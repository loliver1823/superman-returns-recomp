#!/usr/bin/env python3
"""query_rag.py — Query the RAG index for similar crashes, stubs, or structs.

Usage:
    python scripts/query_rag.py --query "vtable crash render pump"
    python scripts/query_rag.py --query "0x82282DF0" --type stubs
    python scripts/query_rag.py --query "PlayerState" --type structs
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.evidence_packet import positive_int, select_text


RAG_DIR = "./rag_index"
COLLECTIONS = ("crash_fixes", "working_stubs", "struct_defs")


def open_client():
    """Load Chroma only for a real retrieval; never create a mistyped index path."""
    if not (Path(RAG_DIR) / "chroma.sqlite3").is_file():
        raise ValueError("RAG index not found at %s (expected chroma.sqlite3)" % RAG_DIR)
    try:
        import chromadb
    except ImportError as error:
        raise RuntimeError("ChromaDB not installed. Run: pip install chromadb sentence-transformers") from error
    return chromadb.PersistentClient(path=RAG_DIR)


def _collections(client, selected=None):
    # Chroma versions have returned either names or Collection objects here.
    available = {item if isinstance(item, str) else item.name for item in client.list_collections()}
    if selected is not None and selected not in available:
        raise ValueError("collection not found: %s" % selected)
    return [name for name in ([selected] if selected else COLLECTIONS) if name in available]


def _rows(result, collection, nested):
    """Validate backend records before treating a missing field as valid evidence."""
    if not isinstance(result, dict):
        raise ValueError("malformed result from %s: expected object" % collection)

    def column(key, required=False):
        value = result.get(key)
        if value is None:
            if required:
                raise ValueError("malformed result from %s: missing %s" % (collection, key))
            return None
        if nested:
            if not isinstance(value, list) or len(value) != 1:
                raise ValueError("malformed result from %s: expected one %s row" % (collection, key))
            value = value[0]
        if not isinstance(value, list):
            raise ValueError("malformed result from %s: %s is not a list" % (collection, key))
        return value

    ids = column("ids", required=True)
    documents = column("documents", required=True)
    metadata, distances = column("metadatas"), column("distances")
    if any(not isinstance(record_id, str) or not record_id for record_id in ids) or len(set(ids)) != len(ids):
        raise ValueError("malformed result from %s: invalid or duplicate IDs" % collection)
    if any(values is not None and len(values) != len(ids) for values in (documents, metadata, distances)):
        raise ValueError("malformed result from %s: column lengths differ" % collection)
    for index, record_id in enumerate(ids):
        document = documents[index]
        meta = metadata[index] if metadata is not None else None
        distance = distances[index] if distances is not None else None
        if not isinstance(document, str) or (meta is not None and not isinstance(meta, dict)):
            raise ValueError("malformed document or metadata for %s/%s" % (collection, record_id))
        if distance is not None and (isinstance(distance, bool) or not isinstance(distance, (int, float)) or not math.isfinite(distance)):
            raise ValueError("malformed distance for %s/%s" % (collection, record_id))
        yield {"collection": collection, "id": record_id, "distance": distance,
               "metadata": meta if meta is not None else {}, "document": document}


def _project(row, *, full=False, section=None, start_line=1, lines=None):
    original = row["document"]
    selected, bounds = select_text(original, section=section, start_line=start_line, lines=lines)
    result = dict(row)
    truncated = not full and len(selected) > 500
    result["document"] = selected[:500] + "..." if truncated else selected
    if full:
        result["retrieval"] = {
            "rag_dir": str(Path(RAG_DIR).resolve()), "collection": row["collection"], "record_id": row["id"],
            "document_sha256": hashlib.sha256(original.encode("utf-8")).hexdigest(),
            "document_chars": len(original), "selected_chars": len(selected), "truncated": truncated,
            "section": section, **bounds,
        }
    return result


def query(query_text: str, collection_name: str = None, n_results: int = 5, *, full=False, client=None):
    """Similarity search; preserve the original 500-character preview by default."""
    if not query_text or not query_text.strip() or n_results < 1:
        raise ValueError("query must be nonempty and result count must be positive")
    client = client if client is not None else open_client()
    rows = []
    for name in _collections(client, collection_name):
        collection = client.get_collection(name)
        count = collection.count()
        if count:
            result = collection.query(query_texts=[query_text], n_results=min(n_results, count))
            rows.extend(_rows(result, name, nested=True))
    rows.sort(key=lambda row: row["distance"] if row["distance"] is not None else math.inf)
    return [_project(row, full=full) for row in rows[:n_results]]


def get_record(record_id, collection_name=None, *, section=None, start_line=1, lines=None, client=None):
    """Fetch the complete stored record by ID without an embedding search.

    Full means the stored document: content already truncated during indexing
    cannot be recovered here. Older clipped diffs require the original git/archive
    artifact; metadata.source_file identifies crash code, not that original diff.
    IDs may exist in multiple collections; every exact match is labelled.
    """
    if not record_id or not record_id.strip():
        raise ValueError("record ID must be nonempty")
    client = client if client is not None else open_client()
    rows = []
    for name in _collections(client, collection_name):
        collection = client.get_collection(name, embedding_function=None)
        result = collection.get(ids=[record_id], include=["documents", "metadatas"])
        for row in _rows(result, name, nested=False):
            if row["id"] != record_id:
                raise ValueError("backend returned an unexpected record ID: %s" % row["id"])
            rows.append(_project(row, full=True, section=section, start_line=start_line, lines=lines))
    if not rows:
        raise ValueError("record ID not found: %s" % record_id)
    return rows


COLLECTION_MAP = {
    "fixes": "crash_fixes",
    "crash_fixes": "crash_fixes",
    "stubs": "working_stubs",
    "working_stubs": "working_stubs",
    "structs": "struct_defs",
    "struct_defs": "struct_defs",
}


def main(argv=None):
    global RAG_DIR
    parser = argparse.ArgumentParser(description="Query RAG index")
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument("--query", help="Search query (500-character previews by default)")
    selector.add_argument("--id", help="Exact record ID; returns full stored documents without embedding search")
    parser.add_argument("--type", default=None, choices=["fixes", "stubs", "structs"],
                        help="Collection to search (default: all)")
    parser.add_argument("--n", type=positive_int, default=5, help="Number of search results")
    parser.add_argument("--rag-dir", default=RAG_DIR, help="RAG index directory")
    parser.add_argument("--full", action="store_true", help="Return complete stored search results and provenance")
    parser.add_argument("--section", help="With --id: exact unique Markdown heading or standalone Label:")
    parser.add_argument("--start-line", type=positive_int, help="With --id: first selected line (one-based)")
    parser.add_argument("--lines", type=positive_int, help="With --id: number of selected lines")
    args = parser.parse_args(argv)
    if args.query is not None and (args.section is not None or args.start_line is not None or args.lines is not None):
        parser.error("section and line selection require --id")
    if args.section is not None and (args.start_line is not None or args.lines is not None):
        parser.error("section and line selection are mutually exclusive")
    RAG_DIR = args.rag_dir

    collection_name = COLLECTION_MAP.get(args.type) if args.type else None
    try:
        results = (get_record(args.id, collection_name, section=args.section,
                              start_line=args.start_line or 1, lines=args.lines)
                   if args.id is not None else query(args.query, collection_name, args.n, full=args.full))
    except Exception as error:
        print("query_rag: %s" % error, file=sys.stderr)
        return 1

    if not results:
        print("No results found.")
        return 0

    print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
