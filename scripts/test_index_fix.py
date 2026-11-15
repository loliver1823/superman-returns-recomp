"""Offline regression tests for complete fix records and file-input provenance."""

import contextlib
import hashlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import index_fix as indexer

ROOT = Path(__file__).resolve().parents[1]


class Collection:
    def __init__(self):
        self.rows = []

    def upsert(self, **kwargs):
        self.rows.append(kwargs)

    def count(self):
        return len(self.rows)


class Client:
    def __init__(self):
        self.collection = Collection()
        self.calls = []

    def get_or_create_collection(self, name, **kwargs):
        self.calls.append((name, kwargs))
        return self.collection


class IndexFixTests(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / "archive" / "evidence_retrieval_tests"
        scratch.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=scratch)
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.log = self.folder / "hang.log"
        self.log.write_text("No crash marker in this hang investigation.\n", encoding="utf-8")
        self.client = Client()
        old_dir = indexer.RAG_DIR
        self.addCleanup(setattr, indexer, "RAG_DIR", old_dir)

    def record(self, *args, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertTrue(indexer.index_fix(str(self.log), "Complete finding", *args, client=self.client, **kwargs))
        row = self.client.collection.rows[-1]
        return row["documents"][0], row["metadatas"][0]

    def test_long_inline_diff_is_preserved_without_2000_character_cap(self):
        diff = "@@ original @@\n" + "+retained line\n" * 5000 + "+EXACT_TAIL\n"
        document, metadata = self.record(diff, "endianness", "guest helper")
        self.assertTrue(document.endswith("Diff:\n" + diff))
        self.assertGreater(metadata["fix_diff_chars"], 2000)
        self.assertEqual(metadata["fix_diff_chars"], len(diff))
        self.assertEqual(metadata["fix_diff_sha256"], hashlib.sha256(diff.encode()).hexdigest())
        self.assertTrue(metadata["fix_diff_complete"])
        self.assertEqual(metadata["fix_diff_origin"], "inline")
        self.assertEqual(metadata["fix_diff_file"], "")
        self.assertEqual(metadata["fix_diff_file_sha256"], "")
        self.assertEqual(metadata["crash_log"], str(self.log.resolve()))

    def test_utf8_file_preserves_crlf_and_separates_raw_and_text_hashes(self):
        diff = "@@ unicode @@\r\n+\u03bb preserved\r\n" + "+tail\r\n" * 400
        raw = b"\xef\xbb\xbf" + diff.encode("utf-8")
        path = self.folder / "fix with spaces.diff"
        path.write_bytes(raw)
        document, metadata = self.record(fix_diff_file=path)
        self.assertTrue(document.endswith("Diff:\n" + diff))
        self.assertEqual(metadata["fix_diff_origin"], "file")
        self.assertEqual(metadata["fix_diff_file"], str(path.resolve()))
        self.assertEqual(metadata["fix_diff_file_sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(metadata["fix_diff_sha256"], hashlib.sha256(diff.encode()).hexdigest())
        self.assertNotEqual(metadata["fix_diff_file_sha256"], metadata["fix_diff_sha256"])
        self.assertEqual(path.read_bytes(), raw)

    def test_crash_source_file_stays_code_location_not_diff_or_log_provenance(self):
        primary = {"guest_lr": "0x82282DF0", "source_file": "recomp/body.cpp",
                   "watchpoint_hits": [{"type": "vtable"}, {"type": "pointer"}]}
        with patch.object(indexer, "parse_crash_log", return_value={"primary_crash": primary, "max_present_reached": 17}):
            document, metadata = self.record("+tail\n")
        self.assertEqual(metadata["source_file"], "recomp/body.cpp")
        self.assertEqual(metadata["crash_log"], str(self.log.resolve()))
        self.assertEqual(metadata["fix_diff_file"], "")
        self.assertEqual(metadata["present_count"], 17)
        self.assertTrue(document.endswith("Diff:\n+tail\n"))
        self.assertLess(document.index("Watchpoint types:"), document.index("Diff:"))

    def test_missing_diff_and_explicit_empty_diff_file_are_distinguished(self):
        document, metadata = self.record()
        self.assertNotIn("Diff:", document)
        self.assertEqual(metadata["fix_diff_origin"], "none")
        self.assertEqual(metadata["fix_diff_chars"], 0)
        path = self.folder / "empty.diff"
        path.write_bytes(b"")
        _, metadata = self.record(fix_diff_file=path)
        self.assertEqual(metadata["fix_diff_origin"], "file")
        self.assertEqual(metadata["fix_diff_file_sha256"], hashlib.sha256(b"").hexdigest())

    def test_invalid_utf8_missing_files_and_conflicting_inputs_never_reach_backend(self):
        path = self.folder / "bad.diff"
        path.write_bytes(b"\xff")
        for options in ({"fix_diff_file": path}, {"fix_diff_file": self.folder / "missing"},
                        {"fix_diff_file": self.folder}, {"fix_diff": "inline", "fix_diff_file": path},
                        {"fix_diff": None}, {"category": "invalid"}):
            with self.subTest(options=options), self.assertRaises((ValueError, OSError)):
                indexer.index_fix(str(self.log), "description", client=self.client, **options)
        for log in (self.folder / "missing.log", self.folder):
            with self.subTest(log=log), self.assertRaises((ValueError, OSError)):
                indexer.index_fix(str(log), "description", client=self.client)
        with self.assertRaises(ValueError):
            indexer.index_fix(str(self.log), "  ", client=self.client)
        self.assertEqual(self.client.calls, [])

    def test_legacy_cli_and_file_cli_preserve_inputs(self):
        path = self.folder / "fix.diff"
        path.write_bytes(b"+file one\r\n+file two\r\n")
        base = ["--crash-log", str(self.log), "--fix-description", "legacy", "--category", "endianness"]
        with patch.object(indexer, "open_client", return_value=self.client), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(indexer.main(base + ["--fix-diff", "+inline one\n+inline two\n"]), 0)
            self.assertEqual(indexer.main(base + ["--fix-diff-file", str(path)]), 0)
        self.assertTrue(self.client.collection.rows[0]["documents"][0].endswith("+inline one\n+inline two\n"))
        self.assertTrue(self.client.collection.rows[1]["documents"][0].endswith("+file one\r\n+file two\r\n"))
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            indexer.main(base + ["--fix-diff", "text", "--fix-diff-file", str(path)])
        self.assertEqual(error.exception.code, 2)

    def test_backend_failure_is_nonzero_and_does_not_print_success(self):
        with patch.object(indexer, "open_client", side_effect=RuntimeError("backend unavailable")), \
                contextlib.redirect_stderr(io.StringIO()) as error, contextlib.redirect_stdout(io.StringIO()) as output:
            result = indexer.main(["--crash-log", str(self.log), "--fix-description", "failure"])
        self.assertEqual(result, 1)
        self.assertIn("backend unavailable", error.getvalue())
        self.assertEqual(output.getvalue(), "")

    def test_help_works_without_chroma_or_repository_working_directory(self):
        result = subprocess.run([sys.executable, "-S", str(ROOT / "scripts" / "index_fix.py"), "--help"],
                                cwd=self.folder, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--fix-diff-file", result.stdout)


if __name__ == "__main__":
    unittest.main()
