"""Exercise retrieval contracts with a fake Chroma backend, without embedding models."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import query_rag as rag

ROOT = Path(__file__).resolve().parents[1]


class Collection:
    def __init__(self, records):
        self.records = records
        self.query_calls = []
        self.get_calls = []

    def count(self):
        return len(self.records)

    @staticmethod
    def result(records, nested=False):
        result = {"ids": [r[0] for r in records], "documents": [r[1] for r in records],
                  "metadatas": [r[2] for r in records], "distances": [r[3] for r in records]}
        return {key: [value] for key, value in result.items()} if nested else result

    def query(self, **kwargs):
        self.query_calls.append(kwargs)
        return self.result(self.records[:kwargs["n_results"]], nested=True)

    def get(self, **kwargs):
        self.get_calls.append(kwargs)
        return self.result([r for r in self.records if r[0] in kwargs["ids"]])


class Client:
    def __init__(self, collections, names_as_objects=False):
        self.collections = collections
        self.calls = []
        self.names_as_objects = names_as_objects

    def list_collections(self):
        return [types.SimpleNamespace(name=name) if self.names_as_objects else name for name in self.collections]

    def get_collection(self, name, **kwargs):
        self.calls.append((name, kwargs))
        return self.collections[name]


class RagTests(unittest.TestCase):
    def setUp(self):
        self.client = Client({"crash_fixes": Collection([
            ("long", "x" * 500 + "TAIL", {"source_file": "source.json"}, 0.2),
            ("boundary", "y" * 500, {}, 0.3),
        ]), "working_stubs": Collection([("best", "stub", {}, 0.1)])})
        self.original_dir = rag.RAG_DIR
        self.addCleanup(setattr, rag, "RAG_DIR", self.original_dir)

    def test_original_query_shape_sorting_limit_and_preview_boundary(self):
        result = rag.query("crash", client=self.client)
        self.assertEqual([r["id"] for r in result], ["best", "long", "boundary"])
        self.assertEqual(set(result[0]), {"collection", "id", "distance", "metadata", "document"})
        self.assertEqual(result[1]["document"], "x" * 500 + "...")
        self.assertEqual(result[2]["document"], "y" * 500)
        self.assertEqual(len(rag.query("crash", n_results=1, client=self.client)), 1)

    def test_full_search_restores_tail_and_reports_source_hash(self):
        result = rag.query("crash", "crash_fixes", full=True, client=self.client)[0]
        self.assertTrue(result["document"].endswith("TAIL"))
        self.assertEqual(result["retrieval"]["document_chars"], 504)
        self.assertFalse(result["retrieval"]["truncated"])
        self.assertEqual(result["retrieval"]["document_sha256"], hashlib.sha256(result["document"].encode()).hexdigest())
        self.assertEqual(result["metadata"]["source_file"], "source.json")

    def test_exact_id_returns_full_without_any_query_or_embedding_function(self):
        result = rag.get_record("long", client=self.client)
        self.assertEqual(result[0]["document"], "x" * 500 + "TAIL")
        self.assertEqual(result[0]["retrieval"]["record_id"], "long")
        self.assertTrue(all(kwargs == {"embedding_function": None} for _, kwargs in self.client.calls))
        for collection in self.client.collections.values():
            self.assertFalse(collection.query_calls)
            self.assertEqual(collection.get_calls[0], {"ids": ["long"], "include": ["documents", "metadatas"]})

    def test_collection_object_names_and_duplicate_id_across_collections(self):
        records = [("same", "full", {}, 0.1)]
        client = Client({"crash_fixes": Collection(records), "working_stubs": Collection(records)}, names_as_objects=True)
        self.assertEqual([r["collection"] for r in rag.get_record("same", client=client)], ["crash_fixes", "working_stubs"])
        self.assertEqual(len(rag.get_record("same", "crash_fixes", client=client)), 1)

    def test_exact_section_and_line_reads_use_complete_stored_document(self):
        source = "Crash: old\n" + "x" * 700 + "\n\nDiff:\n+retained\n"
        client = Client({"crash_fixes": Collection([("fix", source, {}, 0)])})
        result = rag.get_record("fix", section="Diff", client=client)[0]
        self.assertEqual(result["document"], "Diff:\n+retained\n")
        self.assertEqual(result["retrieval"]["first_line"], 4)
        self.assertEqual(result["retrieval"]["document_chars"], len(source))
        result = rag.get_record("fix", start_line=5, lines=1, client=client)[0]
        self.assertEqual(result["document"], "+retained\n")
        with self.assertRaises(ValueError):
            rag.get_record("fix", section="Absent", client=client)

    def test_absent_id_explicit_collection_and_backend_failures_are_errors(self):
        with self.assertRaisesRegex(ValueError, "record ID not found"):
            rag.get_record("absent", client=self.client)
        with self.assertRaisesRegex(ValueError, "collection not found"):
            rag.query("query", "struct_defs", client=self.client)
        with patch.object(self.client, "get_collection", side_effect=RuntimeError("offline")), self.assertRaisesRegex(RuntimeError, "offline"):
            rag.query("query", client=self.client)
        self.assertEqual(rag.query("query", client=Client({})), [])

    def test_malformed_backend_results_are_rejected(self):
        malformed = ({"ids": ["id"]}, {"ids": ["id"], "documents": [None]},
                     {"ids": ["id"], "documents": []}, {"ids": ["id", "id"], "documents": ["a", "b"]},
                     {"ids": ["id"], "documents": ["a"], "metadatas": ["wrong"]},
                     {"ids": ["id"], "documents": ["a"], "distances": [float("nan")]},
                     {"ids": ["id"], "documents": ["a"], "distances": [True]})
        for result in malformed:
            with self.subTest(result=result), self.assertRaises(ValueError):
                list(rag._rows(result, "test", nested=False))
        with self.assertRaises(ValueError):
            list(rag._rows({"ids": [[], []], "documents": [[]]}, "test", nested=True))
        with patch.object(self.client.collections["crash_fixes"], "get", return_value=Collection.result([("wrong", "doc", {}, None)])), self.assertRaisesRegex(ValueError, "unexpected record ID"):
            rag.get_record("id", client=self.client)

    def test_unknown_distance_sorts_last_and_none_metadata_is_safe(self):
        client = Client({"crash_fixes": Collection([("unknown", "doc", None, None)]),
                         "working_stubs": Collection([("known", "doc", {}, 1)])})
        result = rag.query("query", client=client)
        self.assertEqual([r["id"] for r in result], ["known", "unknown"])
        self.assertEqual(result[1]["metadata"], {})

    def test_missing_index_is_not_created_and_help_has_no_chroma_dependency(self):
        scratch = ROOT / "archive" / "evidence_retrieval_tests"
        scratch.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch) as folder:
            rag.RAG_DIR = str(Path(folder) / "absent")
            with self.assertRaisesRegex(ValueError, "index not found"):
                rag.open_client()
            self.assertFalse(Path(rag.RAG_DIR).exists())
        result = subprocess.run([sys.executable, "-S", str(ROOT / "scripts" / "query_rag.py"), "--help"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--id", result.stdout)

    def test_cli_exact_json_and_failures(self):
        with patch.object(rag, "open_client", return_value=self.client), contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(rag.main(["--id", "long", "--type", "fixes"]), 0)
        self.assertTrue(json.loads(output.getvalue())[0]["document"].endswith("TAIL"))
        with patch.object(rag, "open_client", return_value=self.client), contextlib.redirect_stderr(io.StringIO()) as error:
            self.assertEqual(rag.main(["--id", "absent"]), 1)
        self.assertIn("not found", error.getvalue())
        for args in (["--query", "q", "--section", "Diff"], ["--id", "a", "--section", "Diff", "--start-line", "1"],
                     ["--query", "q", "--n", "0"]):
            with self.subTest(args=args), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
                rag.main(args)
            self.assertEqual(error.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
