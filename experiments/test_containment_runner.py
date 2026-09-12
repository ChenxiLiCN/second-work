"""Server runner safety and report-validation tests; no real dataset runs."""
import json
from pathlib import Path
import struct
import tempfile
import unittest

from audit_semantics import write_hin
from run_server_containment import find_normalized, snapshot, validate_structure


class ContainmentRunnerTests(unittest.TestCase):
    def test_reuse_only_complete_normalized_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            data, results = root / "data", root / "results"
            results.mkdir()
            self.assertIsNone(find_normalized("imdb_large", data, results))
            old = results / "20260910-anchor-x/imdb_large"
            write_hin(old / "normalized", [2, 1], [(0, 1, [(0, 0), (1, 0)])])
            self.assertIsNone(find_normalized("imdb_large", data, results))
            (old / "data-audit.json").write_text(json.dumps(dict(dataset="imdb_large")))
            self.assertEqual(find_normalized("imdb_large", data, results), (old / "normalized").resolve())
            current = data / "derived/normalized/imdb_large"
            write_hin(current, [2, 1], [(0, 1, [(0, 0)])])
            self.assertEqual(find_normalized("imdb_large", data, results), current.resolve())

    def test_snapshot_detects_changed_edges(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write_hin(root / "hin", [2, 1], [(0, 1, [(0, 0)])])
            before = snapshot(root / "hin")
            (root / "hin/edge/0.txt").write_text("0 1 1\n1 0\n")
            self.assertNotEqual(snapshot(root / "hin"), before)

    def test_forest_report_and_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            file = root / "relation-0-0.rcf"
            data = (b"RCFORE01" + struct.pack("<4I4Q", 0, 0, 0, 1, 3, 2, 1, 1)
                    + struct.pack("<2I", 2, 0) + struct.pack("<6I", 2, 1, 2**32-1, 0, 0, 1))
            file.write_bytes(data)
            view = dict(relation=0, direction=0, source_type=0, target_type=1,
                        source_vertices=3, nonempty_rows=1, classes=1, parent_edges=0,
                        forest_bytes=len(data), file=file.name)
            report = dict(complete=True, diagnostic_only=True, query_inputs_used=False,
                          online_speedup_measured=False, views=[view], forest_bytes=len(data))
            self.assertEqual(validate_structure(root, report)[0]["bytes"], 88)
            for key, value in (("complete", False), ("query_inputs_used", True),
                               ("online_speedup_measured", True), ("forest_bytes", 89)):
                with self.assertRaises(ValueError):
                    validate_structure(root, dict(report, **{key: value}))
            view["file"] = "../other.rcf"
            with self.assertRaises(ValueError):
                validate_structure(root, report)
            view["file"] = file.name
            file.write_bytes(data[:-1])
            with self.assertRaises(ValueError):
                validate_structure(root, report)


if __name__ == "__main__":
    unittest.main()
