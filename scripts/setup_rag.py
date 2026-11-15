#!/usr/bin/env python3
"""Incrementally index complete generated functions and headers in the existing RAG.

  python scripts/setup_rag.py --dry-run
  python scripts/setup_rag.py --only stubs --max-upserts 512
  python scripts/setup_rag.py --only stubs --partial --offline --batch-size 16

Only this version's records for the exact source root are reconciled. Legacy,
manual, other-root and crash_fixes records are untouched. Missing/unreadable roots
are errors, never an empty inventory. Headers are retained whole, including nested
types and trailing declarations. Embedding batches are bounded; the default refuses
more than 512 new/changed documents before any record mutation. --dry-run plans
without creating collections or changing records (Chroma still opens its store).
--partial applies at most --max-upserts pending documents in deterministic source
order and defers all removals until a complete pass. --offline uses only an already
cached MiniLM model, two CPU threads, and at most 16 documents per inference batch.
Its embeddings see the first 256 tokens; retained source documents remain complete.
"""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from va_to_source import FUNC_RE

RAG_DIR = "./rag_index"
OWNER = "sr-setup-rag/2"
MAX_SOURCE_BYTES = 512 * 1024 * 1024
MAX_FILE_BYTES = 64 * 1024 * 1024
COLLECTION_NAMES = {"stubs": "working_stubs", "headers": "struct_defs"}

# Skip comments/literals before recognizing headers and balancing body braces.
# FUNC_RE is shared with the VA index; no second spelling of generated functions.
TOKENS = re.compile(
    r'//[^\r\n]*|/\*[\s\S]*?\*/|'
    r'R"(?P<delimiter>[^()\s\\]{0,16})\([\s\S]*?\)(?P=delimiter)"|'
    r'"(?:\\[\s\S]|[^"\\])*"|(?<!\w)(?:u8|[uUL])?\'(?:\\[\s\S]|[^\'\\\r\n])*\'|'
    r'(?P<function>' + FUNC_RE.pattern + r')\s*\{|(?P<brace>[{}])'
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def get_client():
    try:
        import chromadb
    except ImportError as error:
        raise RuntimeError("ChromaDB not installed. Run: pip install chromadb sentence-transformers") from error
    from chromadb.config import Settings
    return chromadb.PersistentClient(path=RAG_DIR, settings=Settings(anonymized_telemetry=False))


class OfflineCollection:
    """Supply local vectors explicitly; Chroma never invokes a provider/download."""
    def __init__(self, collection):
        provider = collection.configuration.get("embedding_function")
        if provider is None or provider.name() not in ("default", "onnx_mini_lm_l6_v2"):
            raise ValueError("--offline supports only the existing default MiniLM embedding provider")
        dimension = collection._model.dimension
        if dimension not in (None, 384):
            raise ValueError("--offline MiniLM requires a 384-dimensional collection")
        self.collection, self.embedder = collection, None

    def get(self, **kwargs):
        return self.collection.get(**kwargs)

    def update(self, **kwargs):
        return self.collection.update(**kwargs)

    def delete(self, **kwargs):
        return self.collection.delete(**kwargs)

    def upsert(self, **kwargs):
        if self.embedder is None:
            self.embedder = cached_embedder()
        embeddings = self.embedder(kwargs["documents"])
        return self.collection.upsert(**kwargs, embeddings=embeddings)


def cached_embedder():
    from chromadb.utils.embedding_functions.onnx_mini_lm_l6_v2 import ONNXMiniLM_L6_V2
    model = ONNXMiniLM_L6_V2(preferred_providers=["CPUExecutionProvider"])
    directory = model.DOWNLOAD_PATH / model.EXTRACTED_FOLDER_NAME
    for name in ("config.json", "model.onnx", "special_tokens_map.json", "tokenizer_config.json", "tokenizer.json", "vocab.txt"):
        if not (directory / name).is_file():
            raise ValueError("--offline requires the complete existing MiniLM cache; missing %s" % (directory / name))
    options = model.ort.SessionOptions()
    options.intra_op_num_threads, options.inter_op_num_threads = 2, 1
    options.log_severity_level = 3
    model.model = model.ort.InferenceSession(str(directory / "model.onnx"), sess_options=options,
                                           providers=["CPUExecutionProvider"])
    # _forward uses only the cached tokenizer/session. Do not call __call__, which
    # contains Chroma's automatic download path even when an input file goes missing.
    return lambda documents: model._forward(documents, batch_size=16)


def split_recomp_file(content):
    """Complete PPC_FUNC_IMPL bodies with exact one-based source line bounds."""
    chunks, active, depth, addresses = [], None, 0, set()
    previous_end, previous_line = 0, 1
    for token in TOKENS.finditer(content):
        if token.group("function") is not None:
            if active is not None:
                raise ValueError("nested generated function header; malformed body")
            address = "0x%08X" % int(FUNC_RE.search(token.group("function")).group(1), 16)
            if address in addresses:
                raise ValueError("duplicate generated function address: %s" % address)
            addresses.add(address)
            start_line = previous_line + content.count("\n", previous_end, token.start())
            active, depth = (token.start(), address, start_line), 1
        elif active is not None and token.group("brace"):
            depth += 1 if token.group("brace") == "{" else -1
            if depth == 0:
                start, address, start_line = active
                end_line = start_line + content.count("\n", start, token.end())
                chunks.append({"address": address, "code": content[start:token.end()],
                               "line_start": start_line, "line_end": end_line})
                previous_end, previous_line = token.end(), end_line
                active = None
    if active is not None:
        raise ValueError("unterminated generated function: %s" % active[1])
    if not chunks:
        raise ValueError("source has no supported PPC_FUNC_IMPL definitions; refusing an unknown whole-file chunk")
    return chunks


def source_files(root, kind):
    if not root.is_dir():
        raise ValueError("source root is missing or is not a directory: %s" % root)
    if kind == "stubs":
        paths = sorted(root.glob("ppc_recomp.*.cpp"))
    elif kind == "headers":
        paths = []
        def fail(error):
            raise error
        for directory, subdirs, names in os.walk(root, followlinks=False, onerror=fail):
            for name in subdirs:
                path = Path(directory) / name
                if path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
                    raise ValueError("source inventory contains a link/reparse directory: %s" % path)
            paths.extend(Path(directory) / name for name in names if Path(name).suffix.lower() in (".h", ".hpp", ".hxx"))
        paths.sort()
    else:
        raise ValueError("unknown source kind: %s" % kind)
    for path in paths:
        if not path.is_file() or path.is_symlink() or getattr(path.lstat(), "st_file_attributes", 0) & 0x400:
            raise ValueError("source is not a regular unlinked file: %s" % path)
    return paths


def scope_for(root, kind):
    return digest((kind + "\0" + os.path.normcase(str(root))).encode("utf-8"))


def source_records(root, kind):
    """Build the desired content inventory before touching collection records."""
    root = Path(root).resolve(strict=True)
    scope = scope_for(root, kind)
    records, manifest, total_bytes = {}, {}, 0
    for path in source_files(root, kind):
        size = path.stat().st_size
        if size > MAX_FILE_BYTES or total_bytes + size > MAX_SOURCE_BYTES:
            raise ValueError("source inventory exceeds file/total byte bounds")
        with path.open("rb") as stream:
            raw = stream.read(MAX_FILE_BYTES + 1)
        total_bytes += len(raw)
        if len(raw) > MAX_FILE_BYTES or total_bytes > MAX_SOURCE_BYTES:
            raise ValueError("source changed beyond byte bounds while reading")
        relative = path.relative_to(root).as_posix()
        manifest[relative] = digest(raw)
        text = raw.decode("utf-8-sig")
        chunks = split_recomp_file(text) if kind == "stubs" else [
            {"address": "header", "code": text, "line_start": 1,
             "line_end": text.count("\n") + (0 if text.endswith("\n") else 1)}]
        for chunk in chunks:
            key = scope + "\0" + relative + "\0" + chunk["address"]
            record_id = "sr-src-v2:" + digest(key.encode("utf-8"))
            metadata = {"indexer": OWNER, "source_scope": scope, "source_root": str(root),
                        "source_file": relative, "source_path": str(path), "source_sha256": manifest[relative],
                        "document_sha256": digest(chunk["code"].encode("utf-8")), "complete_document": True,
                        "line_start": chunk["line_start"], "line_end": chunk["line_end"], "line_base": 1,
                        "type": "recomp_stub" if kind == "stubs" else "header"}
            if kind == "stubs":
                metadata["guest_address"] = chunk["address"]
            records[record_id] = {"document": chunk["code"], "metadata": metadata}
    return root, scope, manifest, records


def _owned(metadata, scope):
    return isinstance(metadata, dict) and metadata.get("indexer") == OWNER and metadata.get("source_scope") == scope


def _metadata_rows(result):
    ids, rows = result.get("ids"), result.get("metadatas")
    if not isinstance(ids, list) or not isinstance(rows, list) or len(ids) != len(rows):
        raise ValueError("malformed collection metadata response")
    if any(not isinstance(key, str) or not key for key in ids) or len(ids) != len(set(ids)):
        raise ValueError("invalid/duplicate collection IDs")
    return zip(ids, rows)


def owned_records(collection, scope):
    if collection is None:
        return {}
    records, offset = {}, 0
    where = {"$and": [{"indexer": OWNER}, {"source_scope": scope}]}
    while True:
        result = collection.get(where=where, include=["metadatas"], limit=1000, offset=offset)
        page = list(_metadata_rows(result))
        for key, metadata in page:
            if not _owned(metadata, scope) or key in records:
                raise ValueError("collection returned foreign or repeated owned records")
            records[key] = metadata
        if len(page) < 1000:
            return records
        offset += len(page)
        if offset > 100000:
            raise ValueError("owned record inventory exceeds 100000 records")


def batches(values, size):
    for index in range(0, len(values), size):
        yield values[index:index + size]


def verify_sources(root, kind, manifest):
    current = {}
    total = 0
    for path in source_files(root, kind):
        size = path.stat().st_size
        total += size
        if size > MAX_FILE_BYTES or total > MAX_SOURCE_BYTES:
            raise ValueError("source verification exceeds byte bounds")
        with path.open("rb") as stream:
            raw = stream.read(MAX_FILE_BYTES + 1)
        if len(raw) > MAX_FILE_BYTES:
            raise ValueError("source verification exceeds file byte bound")
        current[path.relative_to(root).as_posix()] = digest(raw)
    if current != manifest:
        raise ValueError("source inventory changed during indexing; no removals performed; retry")


def sync_collection(root, kind, collection, *, dry_run=False, batch_size=128, max_upserts=512, partial=False):
    if batch_size < 1 or max_upserts < 0:
        raise ValueError("batch size must be positive; max_upserts must be nonnegative")
    root, scope, manifest, desired = source_records(root, kind)
    existing = owned_records(collection, scope)
    changed, metadata_only, unchanged = [], [], 0
    for key, row in desired.items():
        old = existing.get(key)
        if old is None or old.get("document_sha256") != row["metadata"]["document_sha256"]:
            changed.append(key)
        elif old != row["metadata"]:
            metadata_only.append(key)
        else:
            unchanged += 1
    removed = sorted(existing.keys() - desired.keys())
    pending = len(changed)
    remaining = max(0, pending - max_upserts) if partial else 0
    deferred_removals = len(removed) if remaining else 0
    if partial:
        changed = changed[:max_upserts]
    if remaining:
        removed = []
    summary = {"schema": "sr-source-index-sync/2", "kind": kind, "source_root": str(root), "source_scope": scope,
               "source_files": len(manifest), "records": len(desired), "upserts": len(changed),
               "metadata_updates": len(metadata_only), "unchanged": unchanged, "removals": len(removed),
               "dry_run": dry_run, "partial": partial, "complete": remaining == 0,
               "pending_upserts": pending, "remaining_upserts": remaining, "deferred_removals": deferred_removals,
               "legacy_manual_and_other_scopes": "preserved"}
    if dry_run:
        return summary
    if len(changed) > max_upserts:
        raise ValueError("plan needs %d document embeddings; --max-upserts %d refuses before record mutation; inspect --dry-run first" % (len(changed), max_upserts))
    if collection is None:
        raise ValueError("a collection is required to apply a plan")
    # An ID collision must never overwrite a manual record carrying our ID spelling.
    new_ids = [key for key in changed if key not in existing]
    for keys in batches(new_ids, batch_size):
        for key, metadata in _metadata_rows(collection.get(ids=keys, include=["metadatas"])):
            if key not in keys or not _owned(metadata, scope):
                raise ValueError("refusing to overwrite a record outside this source ownership scope")
    verify_sources(root, kind, manifest)
    for keys in batches(changed, batch_size):
        collection.upsert(ids=keys, documents=[desired[key]["document"] for key in keys],
                          metadatas=[desired[key]["metadata"] for key in keys])
    for keys in batches(metadata_only, batch_size):
        collection.update(ids=keys, metadatas=[desired[key]["metadata"] for key in keys])
    # Chroma batches are not a transaction: retain old records if any write or source
    # verification fails. A rerun converges; never advertise a partial run as complete.
    verify_sources(root, kind, manifest)
    for keys in batches(removed, batch_size):
        collection.delete(ids=keys)
    return summary


def index_stubs(stubs_dir, collection, **kwargs):
    return sync_collection(stubs_dir, "stubs", collection, **kwargs)


def index_headers(includes_dir, collection, **kwargs):
    return sync_collection(includes_dir, "headers", collection, **kwargs)


@contextmanager
def index_lock(directory):
    path = Path(directory) / ".setup_rag.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    try:
        os.write(fd, str(os.getpid()).encode("ascii"))
        os.close(fd)
        fd = None
        yield
    finally:
        if fd is not None:
            os.close(fd)
        path.unlink()


def integer(minimum):
    def parse(value):
        result = int(value)
        if result < minimum:
            raise argparse.ArgumentTypeError("must be at least %d" % minimum)
        return result
    return parse


def main(argv=None):
    global RAG_DIR
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--stubs-dir", default="guest/ppc")
    parser.add_argument("--includes-dir", default="include")
    parser.add_argument("--rag-dir", default=RAG_DIR)
    parser.add_argument("--only", choices=("stubs", "headers"))
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--batch-size", type=integer(1), default=128)
    parser.add_argument("--max-upserts", type=integer(0), default=512)
    parser.add_argument("--partial", action="store_true", help="apply a bounded pending batch; defer pruning until complete")
    parser.add_argument("--offline", action="store_true", help="cached MiniLM only, CPU threads=2; never download a model")
    args = parser.parse_args(argv)
    RAG_DIR = args.rag_dir
    selected = {"stubs": args.stubs_dir, "headers": args.includes_dir}
    if args.only:
        selected = {args.only: selected[args.only]}
    elif not Path(args.includes_dir).exists() and args.includes_dir == "include":
        # Preserve the old optional default header-root behavior; explicit missing
        # roots and --only headers must fail instead of reconciling against nothing.
        selected.pop("headers")
    try:
        for root in selected.values():
            if not Path(root).is_dir():
                raise ValueError("source root is missing or is not a directory: %s" % root)
        if args.dry_run and not (Path(RAG_DIR) / "chroma.sqlite3").is_file():
            results = [sync_collection(root, kind, None, dry_run=True, partial=args.partial,
                                       max_upserts=args.max_upserts) for kind, root in selected.items()]
        else:
            with index_lock(RAG_DIR):
                client = get_client()
                available = {item if isinstance(item, str) else item.name for item in client.list_collections()}
                results = []
                for kind, root in selected.items():
                    name = COLLECTION_NAMES[kind]
                    collection = client.get_collection(name) if name in available else None
                    if collection is None and not args.dry_run:
                        # Check embedding budget before creating an empty collection.
                        plan = sync_collection(root, kind, None, dry_run=True)
                        if plan["upserts"] > args.max_upserts and not args.partial:
                            raise ValueError("plan needs %d embeddings; raise --max-upserts explicitly after reviewing --dry-run" % plan["upserts"])
                        collection = client.create_collection(name, metadata={"hnsw:space": "cosine"})
                    if args.offline and collection is not None and not args.dry_run:
                        collection = OfflineCollection(collection)
                    results.append(sync_collection(root, kind, collection, dry_run=args.dry_run,
                                                   batch_size=args.batch_size, max_upserts=args.max_upserts,
                                                   partial=args.partial))
                    if args.offline:
                        results[-1]["embedding"] = {"model": "all-MiniLM-L6-v2", "cached_only": True,
                                                   "cpu_threads": 2, "token_limit": 256,
                                                   "complete_document_retained": True}
        print(json.dumps(results, indent=2))
        return 0
    except Exception as error:
        print("setup_rag: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
