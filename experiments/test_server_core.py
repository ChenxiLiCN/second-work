"""Mocked runner checks; these do not measure algorithm performance."""
import contextlib
import csv
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import run_server_core as runner


class RunnerTests(unittest.TestCase):
    def exercise(self, fault=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            raw = root / "data/raw/hin_text/dblp_small"
            raw.mkdir(parents=True)
            (raw / "base.txt").write_text("mock dataset")
            calls = []

            def measured(prefix, command, limit):
                executable = Path(command[0]).name
                calls.append(executable)
                metric = dict(status=0, elapsed_s=2.0, max_rss_kb="100")
                if executable == "verify_adaptive_fli":
                    metric["all_passed"] = "1"
                elif executable.startswith("bri_build"):
                    Path(command[-1]).write_bytes(executable.encode())
                elif executable == "pscan_baseline":
                    (Path(command[1]) / "result-0.5-5.txt").write_text("clusters")
                    if fault == "baseline":
                        metric["status"] = 124
                elif executable.startswith("bri_query"):
                    metric.update(block_mode="9" if "connectivity" in executable else "6",
                                  cache_budget_mib="32", used_roundtrip_metadata=
                                  str(int(Path(command[1]).stem == "enhanced")))
                    output = Path(command[-1])
                    (output / "result-0.5-5.txt").write_text("clusters")
                    (output / "roles-0.5-5.txt").write_text("roles")
                    if prefix.name == "combined":
                        if fault == "roles":
                            (output / "roles-0.5-5.txt").write_text("incorrect roles")
                        elif fault == "timeout":
                            metric["status"] = 124
                        elif fault == "config":
                            metric["used_roundtrip_metadata"] = "0"
                prefix.with_suffix(".log").write_text("mock log")
                prefix.with_suffix(".time").write_text("max_rss_kb=100")
                return metric

            with patch.object(sys, "argv", ["runner", "--suite", "smoke"]), \
                 patch.dict(os.environ, {"DATA_ROOT": str(root / "data"),
                                         "BUILD_DIR": str(root / "build"),
                                         "RESULT_ROOT": str(root / "results")}), \
                 patch.object(runner, "checked"), patch.object(runner, "measured", measured), \
                 patch.object(runner.subprocess, "run", return_value=
                              subprocess.CompletedProcess([], 0, "mock", "")), \
                 contextlib.redirect_stdout(io.StringIO()):
                code = runner.main()
            run = next(p for p in (root / "results").iterdir() if p.is_dir())
            with (run / "runs.tsv").open(newline="") as f:
                rows = list(csv.DictReader(f, delimiter="\t"))
            self.assertEqual(len(rows), 4)
            self.assertEqual(calls.count("bri_build_index"), 1)
            self.assertEqual(calls.count("bri_build_core_index"), 1)
            self.assertEqual(calls.count("hin_materialize"), 1)
            self.assertEqual(calls.count("pscan_baseline"), 1)
            self.assertEqual(sum(c.startswith("bri_query") for c in calls), 4)
            status = json.loads((run / "status.json").read_text())
            self.assertEqual(status["all_passed"], fault is None)
            self.assertEqual(code, int(fault is not None))
            with tarfile.open(str(run) + ".tar.gz") as tar:
                names = tar.getnames()
                self.assertTrue(any(n.endswith("summary.tsv") for n in names))
                self.assertFalse(any(n.endswith(".bri") for n in names))
            if fault is None:
                self.assertTrue(all(r["valid"] == "1" for r in rows))
                self.assertTrue(all(float(r["speedup_vs_hinscan"]) == 2 for r in rows))
            else:
                invalid = [r for r in rows if r["valid"] == "0"]
                self.assertTrue(invalid)
                self.assertTrue(all(not r.get("speedup_vs_hinscan") for r in invalid))

    def test_success(self):
        self.exercise()

    def test_role_mismatch(self):
        self.exercise("roles")

    def test_timeout(self):
        self.exercise("timeout")

    def test_baseline_failure(self):
        self.exercise("baseline")

    def test_wrong_metadata_route(self):
        self.exercise("config")


if __name__ == "__main__":
    unittest.main()
