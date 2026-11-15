#!/usr/bin/env python3
"""
Index key Xenia source files into the struct_defs RAG collection.

Xenia has authoritative implementations of every XDK kernel function.
This script chunks them by DECLARE_XBOXKRNL_EXPORT function blocks and
adds them to the RAG so query_rag.py can find relevant implementations
when investigating stubs.

Usage:
  python scripts/index_xenia.py [--xenia-root C:/Tools/xenia]
"""

import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Key Xenia source directories to index
XENIA_KEY_DIRS = [
    "src/xenia/kernel/xboxkrnl",   # XDK kernel exports
    "src/xenia/kernel/xam",        # Xbox App Manager
    "src/xenia/gpu",               # GPU / command processor
    "src/xenia/apu",               # Audio processing
    "src/xenia/cpu",               # CPU emulation / threading
]

# Only index these file extensions
EXTENSIONS = {".cc", ".h", ".cpp"}

# Split on function boundaries — Xenia uses DECLARE_XBOXKRNL_EXPORT macros
_FUNC_SPLIT = re.compile(
    r'((?:dword_result_t|void|int|pointer_result_t|qword_result_t|float_result_t|double_result_t)'
    r'\s+\w+_entry\s*\([^{]*\)\s*\{[^}]*(?:\{[^}]*\}[^}]*)*\})',
    re.DOTALL
)

_EXPORT_DECL = re.compile(r'DECLARE_XBOXKRNL_EXPORT\d*\s*\(\s*(\w+)\s*,')


def split_by_function(content: str, filepath: Path) -> list[dict]:
    """Split Xenia source into per-function chunks."""
    chunks = []

    # Find all export declarations to get function names
    export_names = set(_EXPORT_DECL.findall(content))

    # Split on _entry functions
    parts = _FUNC_SPLIT.findall(content)

    if not parts:
        # If no function matches, index the whole file (headers, small files)
        if len(content) < 8000:
            return [{
                'name': filepath.stem,
                'code': content,
                'type': 'xenia_source',
            }]
        # Large files without function matches: skip (they're not useful)
        return []

    for func in parts:
        # Extract function name
        name_match = re.search(r'(\w+)_entry\s*\(', func)
        name = name_match.group(1) if name_match else filepath.stem

        chunks.append({
            'name': name,
            'code': func[:6000],  # cap at 6k chars
            'type': 'xenia_impl',
        })

    return chunks


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--xenia-root', default='C:/Tools/xenia',
                    help='Path to Xenia source root (default: C:/Tools/xenia)')
    args = ap.parse_args()

    xenia_root = Path(args.xenia_root)
    if not xenia_root.exists():
        print(f"ERROR: Xenia root not found: {xenia_root}", file=sys.stderr)
        print("Clone with: git clone --depth=1 https://github.com/xenia-project/xenia C:/Tools/xenia")
        return 1

    try:
        import chromadb
    except ImportError:
        print("ChromaDB not installed. Run: pip install chromadb sentence-transformers")
        return 1

    sys.path.insert(0, str(ROOT / 'scripts'))
    from setup_rag import get_client, get_collections

    client = get_client()
    collections = get_collections(client)
    coll = collections['structs']

    total = 0
    for rel_dir in XENIA_KEY_DIRS:
        src_dir = xenia_root / rel_dir
        if not src_dir.exists():
            print(f"  [skip] {rel_dir} not found")
            continue

        for path in sorted(src_dir.rglob('*')):
            if path.suffix not in EXTENSIONS or not path.is_file():
                continue

            try:
                content = path.read_text(encoding='utf-8', errors='replace')
            except OSError:
                continue

            chunks = split_by_function(content, path)
            if not chunks:
                continue

            for chunk in chunks:
                doc_id = hashlib.md5(
                    f"xenia:{path.relative_to(xenia_root)}:{chunk['name']}".encode()
                ).hexdigest()
                coll.upsert(
                    documents=[chunk['code']],
                    ids=[doc_id],
                    metadatas=[{
                        'source_file': str(path.relative_to(xenia_root)),
                        'struct_name': chunk['name'],
                        'type': chunk['type'],
                    }]
                )
                total += 1

        print(f"  {rel_dir}: done")

    print(f"\nIndexed {total} Xenia function chunks into struct_defs collection")
    return 0


if __name__ == '__main__':
    sys.exit(main())
