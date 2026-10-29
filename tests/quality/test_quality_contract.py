"""Fixture integrity and scoring tests. These do not claim model quality."""
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("qctsm_run_quality", ROOT / "run_quality.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class QualityContractTests(unittest.TestCase):
    def test_frozen_fixture_hashes_and_labels(self):
        manifest = json.loads((ROOT / "fixture-manifest.json").read_text(encoding="utf-8"))
        for name in ("catalog", "queries"):
            self.assertEqual(hashlib.sha256((ROOT / f"{name}.json").read_bytes()).hexdigest(), manifest[f"{name}_sha256"])
        catalog = json.loads((ROOT / "catalog.json").read_text(encoding="utf-8"))
        dataset = json.loads((ROOT / "queries.json").read_text(encoding="utf-8"))
        available = {r["capability_id"] for r in catalog["records"]}
        self.assertGreaterEqual(len(dataset["queries"]), 50)
        self.assertGreater(len(available), dataset["top_n"])
        self.assertEqual({q["language"] for q in dataset["queries"]}, {"zh", "en", "mixed"})
        self.assertEqual(len({q["id"] for q in dataset["queries"]}), len(dataset["queries"]))
        for query in dataset["queries"]:
            self.assertTrue(query["expected"])
            self.assertTrue(set(query["expected"]).issubset(available), query["id"])

    def test_user_example_preserves_order_and_has_no_personal_paths(self):
        example = json.loads((ROOT.parents[1] / "examples/gdalwarp-call.json").read_text(encoding="utf-8"))
        self.assertEqual(example["binary_id"], "bin.gdalwarp")
        self.assertEqual(example["arguments"][:7], ["--config", "GDAL_PAM_ENABLED", "NO", "-of", "GTiff", "-r", "lanczos"])
        self.assertEqual(example["arguments"].count("-co"), 4)
        self.assertNotIn("/Users/", json.dumps(example))
        self.assertNotIn("xiaob", json.dumps(example))

    def test_rank_and_recall_use_identifiers_not_descriptions(self):
        query = {"expected": ["native:buffer", "native:clip"]}
        result = runner.score(query, [
            {"capability_id": "something-else", "description": "native:buffer native:clip"},
            {"capability_id": "processing:vector:native:buffer"},
        ], 8)
        self.assertEqual(result["recall"], 0.5)
        self.assertEqual(result["reciprocal_rank"], 0.5)
        self.assertEqual(result["missing"], ["native:clip"])

    def test_rank_cutoff_and_duplicate_do_not_inflate_score(self):
        query = {"expected": ["native:buffer", "native:clip"]}
        candidates = [{"capability_id": "native:buffer"}] * 8 + [{"capability_id": "native:clip"}]
        self.assertEqual(runner.score(query, candidates, 8)["recall"], 0.5)

    def test_native_id_can_be_read_from_payload(self):
        query = {"expected": ["bin.gdalwarp"]}
        result = runner.score(query, [{"capability_id": "opaque-uuid", "payload": {"binary_id": "bin.gdalwarp"}}], 8)
        self.assertEqual(result["recall"], 1.0)

    def test_endpoint_cannot_hide_credentials(self):
        with self.assertRaises(ValueError):
            runner.Client("https://name:secret@example.com", "", 5)


if __name__ == "__main__":
    unittest.main()
