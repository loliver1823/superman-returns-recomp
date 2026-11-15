"""Bounded indexing controls; fixtures stay under this worktree's archive directory."""

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import sqlite3
from contextlib import closing
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import setup_rag as rag
import va_to_source as va

ROOT = Path(__file__).resolve().parents[1]


def function(address, body="return;"):
    return "PPC_FUNC_IMPL(__imp__sub_%08X) {\n%s\n}\n" % (address, body)


class Collection:
    def __init__(self):
        self.rows, self.calls = {}, []

    def get(self, ids=None, where=None, include=None, limit=None, offset=0):
        keys = list(self.rows) if ids is None else [key for key in ids if key in self.rows]
        if where:
            predicates = where["$and"]
            keys = [key for key in keys if all(all(self.rows[key]["metadata"].get(k) == v for k, v in p.items()) for p in predicates)]
        keys = keys[offset:offset + limit if limit is not None else None]
        return {"ids": keys, "metadatas": [dict(self.rows[key]["metadata"]) for key in keys]}

    def upsert(self, ids, documents, metadatas):
        self.calls.append(("upsert", list(ids)))
        for key, document, metadata in zip(ids, documents, metadatas):
            self.rows[key] = {"document": document, "metadata": dict(metadata)}

    def update(self, ids, metadatas):
        self.calls.append(("update", list(ids)))
        for key, metadata in zip(ids, metadatas):
            self.rows[key]["metadata"] = dict(metadata)

    def delete(self, ids):
        self.calls.append(("delete", list(ids)))
        for key in ids:
            del self.rows[key]


class SourceFixture(unittest.TestCase):
    def setUp(self):
        base = ROOT / "archive" / "test_source_index"
        base.mkdir(exist_ok=True, parents=True)
        self.temp = tempfile.TemporaryDirectory(dir=base)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sources = self.root / "sources"
        self.sources.mkdir()
        self.collection = Collection()

    def write(self, text, name="ppc_recomp.0.cpp"):
        path = self.sources / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        return path


class SourceIndexTests(SourceFixture):
    def test_partial_batches_resume_deterministically_and_defer_all_pruning(self):
        path = self.write(function(0x82000000))
        rag.index_stubs(self.sources, self.collection)
        obsolete = set(self.collection.rows)
        self.collection.rows["manual"] = {"document": "keep", "metadata": {"manual": True}}
        path.write_bytes("".join(function(0x82000010 + i * 4) for i in range(5)).encode())
        _, _, _, desired = rag.source_records(self.sources, "stubs")
        order = list(desired)
        self.collection.calls.clear()
        plan = rag.index_stubs(self.sources, self.collection, partial=True, max_upserts=2, dry_run=True)
        self.assertEqual((plan["upserts"], plan["remaining_upserts"], plan["removals"], plan["deferred_removals"]), (2, 3, 0, 1))
        self.assertFalse(plan["complete"])
        self.assertEqual(self.collection.calls, [])
        first = rag.index_stubs(self.sources, self.collection, partial=True, max_upserts=2)
        self.assertFalse(first["complete"])
        self.assertTrue(obsolete <= self.collection.rows.keys())
        self.assertEqual(self.collection.calls, [("upsert", order[:2])])
        self.collection.calls.clear()
        second = rag.index_stubs(self.sources, self.collection, partial=True, max_upserts=2)
        self.assertEqual(second["remaining_upserts"], 1)
        self.assertEqual(self.collection.calls, [("upsert", order[2:4])])
        self.assertTrue(obsolete <= self.collection.rows.keys())
        last = rag.index_stubs(self.sources, self.collection, partial=True, max_upserts=2)
        self.assertTrue(last["complete"])
        self.assertEqual((last["upserts"], last["removals"], last["remaining_upserts"]), (1, 1, 0))
        self.assertEqual(set(self.collection.rows), set(desired) | {"manual"})
        self.collection.calls.clear()
        self.assertTrue(rag.index_stubs(self.sources, self.collection, partial=True, max_upserts=2)["complete"])
        self.assertEqual(self.collection.calls, [])

    def test_zero_partial_budget_neither_embeds_nor_prunes_pending_sources(self):
        path = self.write(function(0x82000000))
        rag.index_stubs(self.sources, self.collection)
        path.write_bytes(function(0x82000010).encode())
        self.collection.calls.clear()
        result = rag.index_stubs(self.sources, self.collection, partial=True, max_upserts=0)
        self.assertFalse(result["complete"])
        self.assertEqual(self.collection.calls, [])

    def test_offline_adapter_supplies_vectors_and_refuses_other_provider_or_dimensions(self):
        class Provider:
            @staticmethod
            def name(): return "default"
        class Model:
            dimension = 384
        class Backend:
            configuration = {"embedding_function": Provider()}
            _model = Model()
            calls = []
            def upsert(self, **kwargs): self.calls.append(kwargs)
        backend = Backend()
        wrapper = rag.OfflineCollection(backend)
        with patch.object(rag, "cached_embedder", return_value=lambda docs: [[1.0, 2.0] for _ in docs]) as make:
            wrapper.upsert(ids=["one"], documents=["full text"], metadatas=[{}])
            wrapper.upsert(ids=["two"], documents=["other text"], metadatas=[{}])
        make.assert_called_once_with()
        self.assertEqual(backend.calls[0]["embeddings"], [[1.0, 2.0]])
        self.assertEqual(backend.calls[0]["documents"], ["full text"])
        with patch.object(Provider, "name", return_value="remote"):
            with self.assertRaisesRegex(ValueError, "only the existing default"):
                rag.OfflineCollection(backend)
        with patch.object(Model, "dimension", 512):
            with self.assertRaisesRegex(ValueError, "384-dimensional"):
                rag.OfflineCollection(backend)

    @unittest.skipUnless(importlib.util.find_spec("chromadb"), "installed Chroma integration")
    def test_offline_missing_cache_fails_without_download_or_model_launch(self):
        from chromadb.utils.embedding_functions.onnx_mini_lm_l6_v2 import ONNXMiniLM_L6_V2
        with patch.object(ONNXMiniLM_L6_V2, "DOWNLOAD_PATH", self.root / "absent-cache"), \
                patch.object(ONNXMiniLM_L6_V2, "_download", side_effect=AssertionError("must never download")), \
                self.assertRaisesRegex(ValueError, "complete existing MiniLM cache"):
            rag.cached_embedder()
        self.assertFalse((self.root / "absent-cache").exists())

    @unittest.skipUnless(importlib.util.find_spec("chromadb"), "installed Chroma integration")
    def test_offline_embedding_bypasses_automatic_download_entrypoint(self):
        from chromadb.utils.embedding_functions import onnx_mini_lm_l6_v2 as module
        from types import SimpleNamespace
        cache = self.root / "cache" / "onnx"
        cache.mkdir(parents=True)
        for name in ("config.json", "model.onnx", "special_tokens_map.json", "tokenizer_config.json", "tokenizer.json", "vocab.txt"):
            (cache / name).write_bytes(b"fixture")
        options = SimpleNamespace()
        calls = []
        class Model:
            DOWNLOAD_PATH, EXTRACTED_FOLDER_NAME = cache.parent, "onnx"
            ort = SimpleNamespace(SessionOptions=lambda: options,
                                  InferenceSession=lambda *a, **k: calls.append((a, k)))
            def __init__(self, **kwargs): pass
            def __call__(self, documents): raise AssertionError("automatic provider/download path called")
            def _forward(self, documents, batch_size):
                self.assert_batch = batch_size
                return [[float(batch_size)]]
        with patch.object(module, "ONNXMiniLM_L6_V2", Model):
            embed = rag.cached_embedder()
            self.assertEqual(embed(["input"]), [[16.0]])
        self.assertEqual((options.intra_op_num_threads, options.inter_op_num_threads), (2, 1))
        self.assertEqual(calls[0][1]["providers"], ["CPUExecutionProvider"])

    def test_balanced_functions_ignore_comments_strings_and_nested_braces(self):
        text = '// PPC_FUNC_IMPL(__imp__sub_81111111) {}\n' + function(0x82000000,
            '/* } PPC_FUNC_IMPL(__imp__sub_82222222) { */\nconst char* s = "}";\nconst char* r = R"tag({})tag";\nif (true) { return; }')
        text += function(0x82000010)
        chunks = rag.split_recomp_file(text)
        self.assertEqual([c["address"] for c in chunks], ["0x82000000", "0x82000010"])
        self.assertEqual(chunks[0]["line_start"], 2)
        self.assertEqual(chunks[1]["code"], function(0x82000010).rstrip("\n"))
        for chunk in chunks:
            self.assertEqual("\n".join(text.splitlines()[chunk["line_start"] - 1:chunk["line_end"]]), chunk["code"])

    def test_numeric_digit_separator_is_not_a_character_literal(self):
        text = function(0x82000000, "auto count = 1'000;\nchar brace = '}';\nwchar_t other = L'{';") + function(0x82000010)
        chunks = rag.split_recomp_file(text)
        self.assertEqual(len(chunks), 2)
        self.assertIn("L'{'", chunks[0]["code"])

    def test_real_generated_file_has_256_individual_functions(self):
        path = ROOT / "guest" / "ppc" / "ppc_recomp.0.cpp"
        if not path.exists():
            self.skipTest("private generated fixture is absent")
        chunks = rag.split_recomp_file(path.read_text(encoding="utf-8"))
        self.assertEqual(len(chunks), 256)
        self.assertEqual(chunks[0]["address"], "0x820F0000")
        self.assertTrue(all(chunk["address"] != "unknown" for chunk in chunks))

    def test_invalid_unterminated_duplicate_and_unsupported_sources_fail(self):
        for text in ("", "// only comments", "void sub_82000000() {}", "PPC_FUNC_IMPL(__imp__sub_82000000) {",
                     function(0x82000000) * 2, "PPC_FUNC_IMPL(__imp__sub_82000000) {" + function(0x82000010)):
            with self.subTest(text=text), self.assertRaises(ValueError):
                rag.split_recomp_file(text)

    def test_batched_initial_noop_changed_and_metadata_only_updates(self):
        path = self.write("".join(function(0x82000000 + i * 4) for i in range(5)))
        result = rag.index_stubs(self.sources, self.collection, batch_size=2)
        self.assertEqual(result["upserts"], 5)
        self.assertEqual([len(keys) for op, keys in self.collection.calls], [2, 2, 1])
        self.collection.calls.clear()
        result = rag.index_stubs(self.sources, self.collection)
        self.assertEqual(result["unchanged"], 5)
        self.assertEqual(self.collection.calls, [])
        before = path.read_text()
        path.write_bytes(("// shifted lines\n" + before.replace("return;", "return 1;", 1)).encode("utf-8"))
        result = rag.index_stubs(self.sources, self.collection)
        self.assertEqual((result["upserts"], result["metadata_updates"]), (1, 4))
        self.assertTrue(all(row["metadata"]["complete_document"] for row in self.collection.rows.values()))

    def test_removals_preserve_legacy_manual_and_other_source_scope_records(self):
        path = self.write(function(0x82000000) + function(0x82000010))
        rag.index_stubs(self.sources, self.collection)
        unrelated = {
            "legacy": {"document": "old", "metadata": {"source_file": path.name}},
            "manual": {"document": "fix", "metadata": {"type": "manual"}},
            "other": {"document": "other root", "metadata": {"indexer": rag.OWNER, "source_scope": "different"}},
        }
        self.collection.rows.update(unrelated)
        path.write_bytes(function(0x82000010).encode("utf-8"))
        result = rag.index_stubs(self.sources, self.collection)
        self.assertEqual(result["removals"], 1)
        path.unlink()
        result = rag.index_stubs(self.sources, self.collection)
        self.assertEqual(result["removals"], 1)
        self.assertEqual(self.collection.rows, unrelated)

    def test_header_full_paths_do_not_collide_and_no_text_is_clipped(self):
        text = "struct Outer { struct Inner { int x; }; };\n" + "// retained\n" * 600 + "extern int EXACT_TAIL;\n"
        self.write(text, "a/common.h")
        self.write(text, "b/common.h")
        result = rag.index_headers(self.sources, self.collection)
        self.assertEqual(result["records"], 2)
        self.assertEqual({row["metadata"]["source_file"] for row in self.collection.rows.values()}, {"a/common.h", "b/common.h"})
        self.assertTrue(all(row["document"] == text for row in self.collection.rows.values()))

    def test_missing_malformed_and_budget_limited_sources_make_no_mutation(self):
        self.write(function(0x82000000))
        for source, options in ((self.root / "missing", {}), (self.sources, {"max_upserts": 0})):
            with self.subTest(source=source), self.assertRaises((ValueError, OSError)):
                rag.index_stubs(source, self.collection, **options)
        self.write("malformed")
        with self.assertRaises(ValueError):
            rag.index_stubs(self.sources, self.collection)
        self.assertEqual(self.collection.calls, [])

    def test_dry_run_has_no_writes_and_collision_never_overwrites_manual_record(self):
        self.write(function(0x82000000))
        result = rag.index_stubs(self.sources, self.collection, dry_run=True, max_upserts=0)
        self.assertEqual(result["upserts"], 1)
        self.assertEqual(self.collection.calls, [])
        _, _, _, records = rag.source_records(self.sources, "stubs")
        key = next(iter(records))
        self.collection.rows[key] = {"document": "manual", "metadata": {"type": "manual"}}
        with self.assertRaisesRegex(ValueError, "outside this source ownership"):
            rag.index_stubs(self.sources, self.collection)
        self.assertEqual(self.collection.rows[key]["document"], "manual")

    def test_source_mutation_after_upsert_blocks_removals(self):
        path = self.write(function(0x82000000) + function(0x82000010))
        rag.index_stubs(self.sources, self.collection)
        path.write_bytes(function(0x82000010, "return 1;").encode("utf-8"))
        self.collection.calls.clear()
        upsert = self.collection.upsert
        def mutate(**kwargs):
            upsert(**kwargs)
            path.write_bytes(function(0x82000010, "return 2;").encode("utf-8"))
        with patch.object(self.collection, "upsert", mutate), self.assertRaisesRegex(ValueError, "changed during indexing"):
            rag.index_stubs(self.sources, self.collection)
        self.assertNotIn("delete", [op for op, _ in self.collection.calls])
        self.assertEqual(len(self.collection.rows), 2)

    def test_same_size_restored_mtime_content_edit_is_still_reembedded(self):
        path = self.write(function(0x82000000, "return 1;"))
        rag.index_stubs(self.sources, self.collection)
        info = path.stat()
        path.write_bytes(path.read_bytes().replace(b"return 1", b"return 2"))
        os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))
        result = rag.index_stubs(self.sources, self.collection)
        self.assertEqual(result["upserts"], 1)
        self.assertIn("return 2;", next(iter(self.collection.rows.values()))["document"])

    def test_backend_failure_retains_obsolete_records_until_successful_retry(self):
        path = self.write(function(0x82000000) + function(0x82000010))
        rag.index_stubs(self.sources, self.collection)
        path.write_bytes(function(0x82000010, "return 1;").encode())
        before = dict(self.collection.rows)
        with patch.object(self.collection, "upsert", side_effect=RuntimeError("fixture backend error")), self.assertRaises(RuntimeError):
            rag.index_stubs(self.sources, self.collection)
        self.assertEqual(self.collection.rows, before)
        result = rag.index_stubs(self.sources, self.collection)
        self.assertEqual((result["upserts"], result["removals"]), (1, 1))

    def test_live_store_help_is_lazy_and_new_store_dry_run_does_not_create_it(self):
        self.write(function(0x82000000))
        destination = self.root / "not-created"
        old = rag.RAG_DIR
        self.addCleanup(setattr, rag, "RAG_DIR", old)
        with patch.object(rag, "get_client", side_effect=AssertionError("must not open Chroma")), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(rag.main(["--only", "stubs", "--stubs-dir", str(self.sources), "--rag-dir", str(destination), "--dry-run"]), 0)
        self.assertFalse(destination.exists())

    @unittest.skipUnless(importlib.util.find_spec("chromadb"), "installed Chroma integration")
    def test_ephemeral_chroma_changes_removals_and_noop_without_model_download(self):
        import chromadb
        from chromadb.config import Settings
        client = chromadb.EphemeralClient(settings=Settings(anonymized_telemetry=False))
        name = "source-fixture-" + self.root.name.lower().replace("_", "-")
        collection = client.create_collection(name, embedding_function=None)
        self.addCleanup(client.delete_collection, name)
        calls = []
        class SuppliedEmbeddings:
            def get(self, **kwargs): return collection.get(**kwargs)
            def update(self, **kwargs):
                calls.append("update")
                return collection.update(**kwargs)
            def delete(self, **kwargs):
                calls.append("delete")
                return collection.delete(**kwargs)
            def upsert(self, **kwargs):
                calls.append("upsert")
                return collection.upsert(**kwargs, embeddings=[[1.0, 0.0] for _ in kwargs["ids"]])
        adapter = SuppliedEmbeddings()
        path = self.write(function(0x82000000) + function(0x82000010))
        collection.add(ids=["manual"], documents=["preserved"], metadatas=[{"manual": True}], embeddings=[[1.0, 0.0]])
        first = rag.index_stubs(self.sources, adapter, partial=True, max_upserts=1)
        self.assertEqual((first["upserts"], first["remaining_upserts"], first["complete"]), (1, 1, False))
        second = rag.index_stubs(self.sources, adapter, partial=True, max_upserts=1)
        self.assertEqual((second["upserts"], second["remaining_upserts"], second["complete"]), (1, 0, True))
        calls.clear()
        self.assertEqual(rag.index_stubs(self.sources, adapter)["unchanged"], 2)
        self.assertEqual(calls, [])
        path.write_bytes(function(0x82000010, "return 3;").encode("utf-8"))
        result = rag.index_stubs(self.sources, adapter)
        self.assertEqual((result["upserts"], result["removals"]), (1, 1))
        stored = collection.get(include=["documents"])
        self.assertEqual(len(stored["ids"]), 2)
        self.assertIn("preserved", stored["documents"])
        self.assertIn(function(0x82000010, "return 3;").rstrip("\n"), stored["documents"])


class VaRefreshTests(SourceFixture):
    def test_matching_fingerprint_without_mapping_tables_is_rebuilt(self):
        self.write(function(0x82000000))
        database = self.root / "meta-only.db"
        with closing(sqlite3.connect(database)) as con:
            con.execute("CREATE TABLE meta(k TEXT PRIMARY KEY,v TEXT)")
            con.execute("INSERT INTO meta VALUES('source_fingerprint',?)", (va.source_fingerprint(self.sources),))
            con.commit()
        result = va.ensure_fresh_index(database, self.sources)
        self.assertEqual(result["status"], "rebuilt")
        with closing(sqlite3.connect(database)) as con:
            self.assertEqual(con.execute("SELECT COUNT(*) FROM funcs").fetchone()[0], 1)

    def test_content_freshness_noop_and_same_size_restored_mtime_edit(self):
        path = self.write(function(0x82000000))
        database = self.root / "va.db"
        first = va.ensure_fresh_index(database, self.sources)
        self.assertEqual(first["status"], "rebuilt")
        before = database.stat().st_mtime_ns
        with patch.object(va, "build_index", side_effect=AssertionError("fresh database rebuilt")):
            self.assertEqual(va.ensure_fresh_index(database, self.sources)["status"], "current")
        self.assertEqual(database.stat().st_mtime_ns, before)
        info = path.stat()
        path.write_bytes(path.read_bytes().replace(b"82000000", b"82000010"))
        os.utime(path, ns=(info.st_atime_ns, info.st_mtime_ns))
        result = va.ensure_fresh_index(database, self.sources)
        self.assertEqual(result["status"], "rebuilt")
        self.assertNotEqual(first["fingerprint"], result["fingerprint"])

    def test_empty_source_and_failed_rebuild_preserve_prior_database(self):
        self.write(function(0x82000000))
        database = self.root / "va.db"
        va.ensure_fresh_index(database, self.sources)
        before = database.read_bytes()
        with patch.object(va, "source_fingerprint", side_effect=["changed", "before", "after"]), self.assertRaises(RuntimeError):
            va.ensure_fresh_index(database, self.sources)
        self.assertEqual(database.read_bytes(), before)
        (self.sources / "ppc_recomp.0.cpp").unlink()
        with self.assertRaises(RuntimeError):
            va.ensure_fresh_index(database, self.sources)
        self.assertEqual(database.read_bytes(), before)


class RegenRefreshTests(SourceFixture):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("regen_index_fixture", ROOT / "tools" / "regen" / "regen.py")
        cls.regen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.regen)

    def test_current_generation_refreshes_index_without_recompiling(self):
        xex = self.root / "private.xex"
        xex.write_bytes(b"fixture")
        with patch.object(sys, "argv", ["regen.py", "--ensure", "--xex", str(xex)]), \
                patch.object(self.regen, "wanted_stamp", return_value="same"), \
                patch.object(self.regen, "have_stamp", return_value="same"), \
                patch.object(self.regen, "bootstrap", side_effect=AssertionError("must not build")), \
                patch.object(self.regen, "refresh_va_index") as refresh, contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(self.regen.main(), 0)
        refresh.assert_called_once_with()

    def test_index_failure_after_publication_does_not_change_generation_identity(self):
        xex, stamp = self.root / "private.xex", self.root / ".regen_stamp"
        xex.write_bytes(b"fixture")
        events = []
        def refreshed():
            self.assertEqual(events, ["deployed"])
            self.assertEqual(stamp.read_text(), "regen v1 wanted\n")
            raise RuntimeError("fixture index failure")
        with patch.object(sys, "argv", ["regen.py", "--force", "--xex", str(xex), "--work", str(self.root / "work")]), \
                patch.object(self.regen, "STAMP", str(stamp)), \
                patch.object(self.regen, "wanted_stamp", return_value="wanted"), \
                patch.object(self.regen, "bootstrap", return_value="fixture"), \
                patch.object(self.regen, "recompile", return_value="fixture"), \
                patch.object(self.regen, "deploy", side_effect=lambda *args: events.append("deployed")), \
                patch.object(self.regen, "refresh_va_index", side_effect=refreshed), \
                self.assertRaisesRegex(RuntimeError, "fixture index failure"):
            self.regen.main()
        self.assertEqual(stamp.read_text(), "regen v1 wanted\n")


if __name__ == "__main__":
    unittest.main()
