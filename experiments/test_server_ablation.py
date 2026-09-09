"""Runner plumbing tests with simulated subprocesses (not algorithm benchmarks)."""
import contextlib
import csv
import importlib.util
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

spec = importlib.util.spec_from_file_location("runner", Path(__file__).with_name("run_server_ablation.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def run_case(self, fault=None):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            index = root / "old/indexes/dblp_small/base.bri"
            index.parent.mkdir(parents=True)
            index.write_bytes(b"test index only")
            calls = []

            def measured(prefix, command, limit):
                name = Path(command[0]).name
                calls.append(name)
                metrics = dict(status=0, elapsed_s=1.25, max_rss_kb="100")
                if name == "verify_adaptive_fli":
                    metrics["all_passed"] = "1"
                else:
                    out = Path(command[-1])
                    for kind in ("result", "roles"):
                        (out / f"{kind}-0.5-5.txt").write_text(kind)
                    if name.startswith("bri_isolated_"):
                        _, _, policy, mib, group = name.split("_")
                        metrics.update(block_mode={"pressure": "6", "off": "7", "always": "8"}[policy],
                                       cache_budget_mib=mib, witness_bound_checks="0" if policy == "off" else "3")
                        if group != "p0":
                            metrics["profile_group"] = group[1:]
                        if fault == "roles" and policy == "always":
                            (out / "roles-0.5-5.txt").write_text("mismatch")
                        if fault == "timeout" and policy == "off":
                            metrics["status"] = 124
                prefix.with_suffix(".log").write_text(json.dumps(metrics))
                prefix.with_suffix(".time").write_text("max_rss_kb=100\n")
                return metrics

            with patch.object(sys, "argv", ["runner", "--suite", "smoke", "--repeats", "2"]), \
                 patch.dict(os.environ, {"RESULT_ROOT": str(root / "results"),
                                          "INDEX_SEARCH_ROOT": str(root),
                                          "BUILD_DIR": str(root / "build")}), \
                 patch.object(runner, "measured", measured), \
                 patch.object(runner, "checked"), \
                 patch.object(runner.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "", "")), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                code = runner.main()
            run = next(p for p in (root / "results").iterdir() if p.is_dir())
            def rows(file):
                with (run / file).open(newline="") as f:
                    return list(csv.DictReader(f, delimiter="\t"))
            self.assertEqual(len(rows("runs.tsv")), 12)
            self.assertEqual(len(rows("diagnostics.tsv")), 12)
            self.assertTrue(all(r["run_kind"] == "timing" for r in rows("runs.tsv")))
            self.assertTrue(all(r["run_kind"] == "diagnostic" for r in rows("diagnostics.tsv")))
            self.assertEqual(index.read_bytes(), b"test index only")
            self.assertNotIn("bri_build_index", calls)
            self.assertEqual(code, 1 if fault else 0)
            self.assertEqual((run / "status.txt").read_text(), "status=failed\n" if fault else "status=passed\n")
            self.assertEqual(len(rows("summary.tsv")), 6)
            self.assertEqual(sum(r["complete"] == "1" for r in rows("summary.tsv")), 4 if fault else 6)
            with tarfile.open(run.with_suffix(".tar.gz")) as tar:
                self.assertFalse(any(n.endswith(".bri") or "/output-" in n for n in tar.getnames()))

    def test_success_and_separate_profiles(self):
        self.run_case()

    def test_role_mismatch_not_valid(self):
        self.run_case("roles")

    def test_timeout_not_valid(self):
        self.run_case("timeout")


if __name__ == "__main__":
    unittest.main()
