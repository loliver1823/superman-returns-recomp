#!/usr/bin/env python3
"""index_fix.py — Index a resolved crash+fix pair into RAG for future reference.

Usage:
    python scripts/index_fix.py --crash-log logs/crash.log --fix-description "Added bswap32 to vtable pointer read in sub_82282DF0"
    python scripts/index_fix.py --crash-log logs/crash.log --fix-description "..." --fix-diff-file archive/fix.diff
    python scripts/index_fix.py --crash-log logs/crash.log --fix-description "..." --category endianness
"""

import argparse
import hashlib
from pathlib import Path
import sys
from datetime import datetime

# Import the crash parser
sys.path.insert(0, str(Path(__file__).resolve().parent))
from parse_crash_log import parse_crash_log


RAG_DIR = "./rag_index"

FIX_CATEGORIES = [
    "endianness",
    "pointer_math",
    "vtable_dispatch",
    "struct_alignment",
    "null_check",
    "signed_unsigned",
    "missing_stub",
    "wrong_offset",
    "other",
]


def open_client():
    try:
        import chromadb
    except ImportError as error:
        raise RuntimeError("ChromaDB not installed. Run: pip install chromadb sentence-transformers") from error
    return chromadb.PersistentClient(path=RAG_DIR)


def index_fix(crash_log_path: str, fix_description: str,
              fix_diff: str = "", category: str = "other",
              hang_signature: str = "", *, fix_diff_file=None, client=None):
    """Index a crash+fix pair OR a hang+investigation pair.

    Many fixes in this project address hangs (not crashes), so we accept logs
    without a `primary_crash`. Pass `--hang-signature` to capture the hang's
    last-known guest LR / function / module so future RAG queries can match it.

    Store the entire supplied diff. Older records may contain only its first
    2,000 characters; source_file names crash code, not an original diff artifact.
    File input accepts UTF-8 with an optional BOM and preserves line endings.
    """
    if not isinstance(fix_description, str) or not fix_description.strip():
        raise ValueError("fix description must be nonempty")
    if category not in FIX_CATEGORIES:
        raise ValueError("unknown fix category: %s" % category)
    if not isinstance(fix_diff, str):
        raise ValueError("fix diff must be text")
    if fix_diff and fix_diff_file is not None:
        raise ValueError("fix_diff and fix_diff_file are mutually exclusive")
    crash_path = Path(crash_log_path).resolve(strict=True)
    if not crash_path.is_file():
        raise ValueError("crash log is not a file: %s" % crash_path)
    diff_path, diff_file_hash = "", ""
    if fix_diff_file is not None:
        path = Path(fix_diff_file).resolve(strict=True)
        raw_diff = path.read_bytes()
        fix_diff = raw_diff.decode("utf-8-sig")
        diff_path = str(path)
        diff_file_hash = hashlib.sha256(raw_diff).hexdigest()
    crash_data = parse_crash_log(str(crash_path))
    primary = crash_data.get("primary_crash") or {}

    is_crash = bool(primary)
    guest_lr = primary.get("guest_lr") if is_crash else (hang_signature or "hang/no-crash")
    present = crash_data.get("max_present_reached", 0)

    if is_crash:
        doc_parts = [
            f"Crash at guest address {guest_lr}",
            f"Exception: {primary.get('exception_code', 'unknown')}",
            f"Fault type: {primary.get('fault_type', 'unknown')}",
            f"Crash type: {primary.get('crash_type', 'unknown')}",
            f"Render state: {primary.get('last_render_state', 'unknown')}",
            f"Present count: {present}",
            f"Fix category: {category}",
            f"Fix: {fix_description}",
        ]
    else:
        doc_parts = [
            f"Hang/investigation: {hang_signature or '(unspecified)'}",
            f"Present count reached: {present}",
            f"Fix category: {category}",
            f"Fix/finding: {fix_description}",
        ]

    # Add watchpoint context
    if primary.get("watchpoint_hits"):
        wp_types = set(wp.get("type", "") for wp in primary["watchpoint_hits"])
        doc_parts.append(f"Watchpoint types: {', '.join(sorted(wp_types))}")

    if fix_diff:
        doc_parts.append(f"Diff:\n{fix_diff}")

    document = "\n".join(doc_parts)

    # Generate a unique ID
    doc_id = hashlib.md5(
        f"fix:{guest_lr}:{present}:{datetime.now().isoformat()}".encode()
    ).hexdigest()

    # Store in RAG
    client = client if client is not None else open_client()
    collection = client.get_or_create_collection(
        "crash_fixes", metadata={"hnsw:space": "cosine"}
    )

    collection.upsert(
        documents=[document],
        ids=[doc_id],
        metadatas=[{
            "guest_lr": guest_lr,
            "exception_code": primary.get("exception_code", "") if is_crash else "",
            "crash_type": primary.get("crash_type", "") if is_crash else "hang_or_finding",
            "fault_type": primary.get("fault_type", "") if is_crash else "",
            "present_count": present,
            "fix_category": category,
            "fix_description": fix_description,
            "hang_signature": hang_signature,
            "date": datetime.now().isoformat(),
            "crash_log": str(crash_path),
            # Existing source_file is the source-code location reported by the crash.
            "source_file": primary.get("source_file", "") if is_crash else "",
            "fix_diff_origin": "file" if fix_diff_file is not None else ("inline" if fix_diff else "none"),
            "fix_diff_file": diff_path,
            "fix_diff_file_sha256": diff_file_hash,
            "fix_diff_sha256": hashlib.sha256(fix_diff.encode("utf-8")).hexdigest(),
            "fix_diff_chars": len(fix_diff),
            "fix_diff_complete": True,
        }]
    )

    print(f"Indexed fix:")
    print(f"  ID:          {doc_id}")
    print(f"  Guest LR:    {guest_lr}")
    print(f"  Present:     #{present}")
    print(f"  Category:    {category}")
    print(f"  Description: {fix_description}")
    print(f"  Total fixes: {collection.count()}")
    return True


def main(argv=None):
    global RAG_DIR
    parser = argparse.ArgumentParser(description="Index a crash+fix pair into RAG")
    parser.add_argument("--crash-log", required=True, help="Path to the crash log")
    parser.add_argument("--fix-description", required=True,
                        help="Human-readable description of the fix")
    diff_input = parser.add_mutually_exclusive_group()
    diff_input.add_argument("--fix-diff", default="", help="Complete diff text (optional)")
    diff_input.add_argument("--fix-diff-file", help="UTF-8 file containing the complete diff; preserves line endings")
    parser.add_argument("--category", default="other", choices=FIX_CATEGORIES,
                        help="Fix category for pattern matching")
    parser.add_argument("--hang-signature", default="",
                        help="Identifier for hangs without a primary_crash (e.g. 'FMHud-CTOR sub_8278D258 1760-byte alloc')")
    parser.add_argument("--rag-dir", default=RAG_DIR, help="RAG index directory")
    args = parser.parse_args(argv)
    RAG_DIR = args.rag_dir

    try:
        success = index_fix(
            args.crash_log, args.fix_description, args.fix_diff, args.category,
            args.hang_signature, fix_diff_file=args.fix_diff_file
        )
    except Exception as error:
        print("index_fix: %s" % error, file=sys.stderr)
        return 1
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
