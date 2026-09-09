#!/usr/bin/env python3
"""One-run factorial experiment: offline roundtrip metadata x core certificate."""
import argparse
import csv
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import tarfile
import tempfile
import time

from run_server_ablation import CASES, ROOT, checked, digest, measured

VARIANTS = [
    ("old", "legacy", "witness_exclusion", "6"),
    ("offline_only", "enhanced", "witness_exclusion", "6"),
    ("certificate_only", "legacy", "core_connectivity", "9"),
    ("combined", "enhanced", "core_connectivity", "9"),
]


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False), encoding="utf-8")


def write_table(path, rows):
    keys = list(dict.fromkeys(key for row in rows for key in row))
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=keys, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def output_hashes(directory, eps):
    return {kind: digest(directory / f"{kind}-{eps}-5.txt")
            for kind in ("result", "roles")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("all", "smoke"), default="all")
    args = parser.parse_args()
    limit = int(os.environ.get("LIMIT_SECONDS", "7200"))
    jobs = int(os.environ.get("BUILD_JOBS", "4"))
    if min(limit, jobs) < 1:
        parser.error("LIMIT_SECONDS and BUILD_JOBS must be positive")
    data = Path(os.environ.get("DATA_ROOT", ROOT / "data")).resolve()
    build = Path(os.environ.get("BUILD_DIR", ROOT / "build-linux-core")).resolve()
    results = Path(os.environ.get("RESULT_ROOT", ROOT / "server-results")).resolve()
    results.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-core-"), dir=results))
    print("Results:", run, flush=True)
    selected = ["dblp_small"] if args.suite == "smoke" else [
        "dblp_apv_full", "dblp_v18", "imdb_legacy", "yelp"]
    rows, offline, errors = [], [], []
    bin_dir = build / "bin"
    try:
        env = dict(experiment="core_factorial_v1", repeats=1, cache_mib=32,
                   platform=platform.platform(), data_root=str(data), build_dir=str(build),
                   limit_seconds=limit, suite=args.suite,
                   timing="Sequential process wall times, including loading and output; no cache flush or warmup; single-run results are exploratory.",
                   baseline="Local HIN materialization (including projected-file output) + upstream pSCAN (including file input/output), NOT official HINSCAN source.",
                   correctness="Independent small-graph oracle; clusters checked against upstream pSCAN; roles checked against old indexed query, not upstream role output.",
                   scope="Roundtrip metadata serves length-two paths only; longer paths retain online construction. No query-path-specific offline inputs.")
        for label, command in [
            ("cpu", ["lscpu"]), ("memory", ["free", "-h"]),
            ("compiler", ["g++", "--version"]),
            ("commit", ["git", "-C", str(ROOT), "rev-parse", "HEAD"]),
            ("diff", ["git", "-C", str(ROOT), "diff", "--", "src", "CMakeLists.txt"]),
        ]:
            p = subprocess.run(command, capture_output=True, text=True)
            env[label] = p.stdout + p.stderr
        env["source_sha256"] = {str(p.relative_to(ROOT)): digest(p)
                                for base in (ROOT / "src", ROOT / "experiments")
                                for p in base.rglob("*") if p.is_file()
                                and p.suffix in (".cpp", ".h", ".py", ".sh")}
        env["source_sha256"]["CMakeLists.txt"] = digest(ROOT / "CMakeLists.txt")
        save_json(run / "environment.json", env)
        for name in selected:
            if not (data / CASES[name][0] / "base.txt").is_file():
                raise RuntimeError(f"Dataset missing: {data / CASES[name][0]}")
        checked(["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_CXX_COMPILER=g++", "-DBUILD_PSCAN_BASELINE=ON"], run / "configure.log")
        checked(["cmake", "--build", build, "--parallel", jobs, "--target",
                 "bri_build_index", "bri_build_core_index", "bri_query_witness_exclusion",
                 "bri_query_core_connectivity", "verify_adaptive_fli", "hin_materialize",
                 "pscan_baseline"], run / "build.log")
        print("Checking independent correctness oracle", flush=True)
        with tempfile.TemporaryDirectory(prefix="oracle-", dir=run) as fixture:
            oracle = measured(run / "oracle", [bin_dir / "verify_adaptive_fli", fixture], limit)
        if oracle["status"] != 0 or oracle.get("all_passed") != "1":
            raise RuntimeError("Correctness oracle failed; see oracle.log")
        for name in selected:
            case = run / name
            case.mkdir()
            raw, path, eps = CASES[name]
            try:
                indexes, index_hashes = {}, {}
                for variant, executable in [("legacy", "bri_build_index"),
                                            ("enhanced", "bri_build_core_index")]:
                    print(f"[{name}] offline {variant} (once)", flush=True)
                    index = case / f"{variant}.bri"
                    metric = measured(case / f"offline-{variant}",
                                      [bin_dir / executable, data / raw, index], limit)
                    row = dict(dataset=name, index=variant, **metric)
                    if metric["status"] == 0:
                        row.update(index_bytes=index.stat().st_size, sha256=digest(index))
                        index_hashes[variant] = row["sha256"]
                    offline.append(row)
                    if metric["status"] != 0:
                        raise RuntimeError(f"{variant} index build failed")
                    indexes[variant] = index
                print(f"[{name}] HINSCAN full-flow reconstruction (once)", flush=True)
                baseline_hash, baseline_s = None, None
                with tempfile.TemporaryDirectory(prefix="projected-", dir=case) as tmp:
                    projected = Path(tmp)
                    mat = measured(case / "materialize",
                                   [bin_dir / "hin_materialize", data / raw, path, projected], limit)
                    scan = None
                    if mat["status"] == 0:
                        scan = measured(case / "pscan",
                                        [bin_dir / "pscan_baseline", projected, eps, "5", "output"], limit)
                    if scan is not None and scan["status"] == 0:
                        baseline_hash = digest(projected / f"result-{eps}-5.txt")
                        baseline_s = mat["elapsed_s"] + scan["elapsed_s"]
                    save_json(case / "baseline.json", dict(materialize=mat, pscan=scan,
                              full_elapsed_s=baseline_s, cluster_sha256=baseline_hash))
                if baseline_s is None:
                    errors.append(f"{name}: full-flow baseline failed; no speedup will be reported")
                # Old query must run first to provide vertex-role reference. Remaining
                # variants use a fixed randomized order; never mix profiling builds.
                order = VARIANTS[1:]
                random.Random(20260909).shuffle(order)
                order = [VARIANTS[0], *order]
                expected, case_rows = None, []
                for position, (label, variant, mode, expected_mode) in enumerate(order, 1):
                    print(f"[{name}] {label} ({position}/4, once)", flush=True)
                    with tempfile.TemporaryDirectory(prefix="query-output-", dir=case) as tmp:
                        output = Path(tmp)
                        metric = measured(case / label,
                            [bin_dir / f"bri_query_{mode}", indexes[variant], path, eps, "5", output], limit)
                        row = dict(dataset=name, meta_path=path, epsilon=eps, mu=5,
                                   variant=label, index=variant, order=position, **metric)
                        hashes = output_hashes(output, eps) if metric["status"] == 0 else None
                        if label == "old":
                            expected = hashes
                        want_metadata = int(variant == "enhanced" and len(path.split("-")) == 3)
                        row["config_match"] = int(metric.get("block_mode") == expected_mode
                            and metric.get("cache_budget_mib") == "32"
                            and metric.get("profile_group", "0") == "0"
                            and metric.get("used_roundtrip_metadata") == str(want_metadata))
                        row["cluster_match_old"] = int(hashes is not None and expected is not None
                                                       and hashes["result"] == expected["result"])
                        row["roles_match_old"] = int(hashes is not None and expected is not None
                                                     and hashes["roles"] == expected["roles"])
                        row["cluster_match_baseline"] = (int(hashes is not None and
                            hashes["result"] == baseline_hash) if baseline_hash else "unavailable")
                        row["valid"] = int(metric["status"] == 0 and row["config_match"] == 1
                            and row["cluster_match_old"] == 1 and row["roles_match_old"] == 1
                            and row["cluster_match_baseline"] == 1)
                        row["baseline_full_s"] = baseline_s
                        if row["valid"]:
                            row["speedup_vs_hinscan"] = baseline_s / row["elapsed_s"]
                        else:
                            errors.append(f"{name}/{label}: failed or unvalidated; see runs.tsv")
                        save_json(case / f"{label}-hashes.json", hashes)
                        case_rows.append(row)
                        rows.append(row)
                for variant, index in indexes.items():
                    if digest(index) != index_hashes[variant]:
                        for row in case_rows:
                            row["valid"] = 0
                            row.pop("speedup_vs_hinscan", None)
                        raise RuntimeError(f"Persistent {variant} index changed during queries")
            except Exception as e:
                errors.append(f"{name}: {e}")
                print(errors[-1], flush=True)
            finally:
                write_table(run / "runs.tsv", rows)
                write_table(run / "offline.tsv", offline)
    except Exception as e:
        errors.append(str(e))
        print("ERROR:", e, flush=True)
    finally:
        write_table(run / "runs.tsv", rows)
        write_table(run / "offline.tsv", offline)
        summary_keys = ("dataset", "meta_path", "variant", "elapsed_s", "baseline_full_s",
                        "speedup_vs_hinscan", "valid", "base_relation_load_ms",
                        "on_demand_fli_build_ms", "core_ms", "noncore_ms", "block_core_vertices")
        write_table(run / "summary.tsv", [{k: r.get(k, "") for k in summary_keys} for r in rows])
        save_json(run / "status.json", dict(all_passed=not errors, errors=errors,
                                           query_runs=len(rows), offline_runs=len(offline)))
        archive = Path(str(run) + ".tar.gz")
        # Package only small evidence files, never indexes or temporary graphs.
        with tarfile.open(archive, "w:gz") as tar:
            for p in sorted(run.rglob("*")):
                if p.is_file() and p.suffix in (".log", ".time", ".json", ".tsv"):
                    tar.add(p, arcname=str(Path(run.name) / p.relative_to(run)))
        print("Archive:", archive, flush=True)
        print("Completed" if not errors else "Completed with failures; check status.json", flush=True)
    return int(bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())
