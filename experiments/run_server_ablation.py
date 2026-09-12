#!/usr/bin/env python3
"""Corrected lazy-activation ablations and separate diagnostic runs; Linux."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "dblp_small": ("raw/hin_text/dblp_small", "A-P-A", "0.5"),
    "dblp_apv_full": ("raw/hin_text/dblp_apv_full", "A-P-A", "0.5"),
    "dblp_v18": ("raw/hin_text/dblp_v18", "A-P-A", "0.5"),
    "imdb_legacy": ("raw/hin_text/imdb_legacy", "A-M-D-M-A", "0.5"),
    "yelp": ("derived/normalized/yelp", "U-R-B-R-U", "0.9"),
}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def fields(path):
    result = {}
    for line in path.read_text(errors="replace").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key] = value
    return result


def reject_unmigrated_semantics(metrics):
    # These historical runners compare against raw pSCAN mu / old role hashes.
    # Fail closed rather than emit a plausible but semantically mixed speedup.
    if metrics.get("semantics_version") or metrics.get("mu_counts_self") == "1":
        raise RuntimeError(
            "Historical experiment runner is not migrated to closed-mu/full-role semantics. "
            "No speedup may be reported. Use verify_hinscan_semantics.py for correctness; "
            "migrate the full-flow baseline and historical-result checks before benchmarking.")


def measured(prefix, command, limit):
    start = time.perf_counter()
    with prefix.with_suffix(".log").open("w") as log:
        p = subprocess.run(
            ["/usr/bin/time", "-f", "max_rss_kb=%M", "-o",
             str(prefix.with_suffix(".time")), "timeout", "--kill-after=30s",
             str(limit) + "s", *map(str, command)], stdout=log,
            stderr=subprocess.STDOUT)
    elapsed = time.perf_counter() - start
    metrics = fields(prefix.with_suffix(".log"))
    reject_unmigrated_semantics(metrics)
    metrics.update(fields(prefix.with_suffix(".time")))
    metrics.update(status=p.returncode, elapsed_s=elapsed)
    return metrics


def checked(command, log):
    with log.open("w") as f:
        subprocess.run(list(map(str, command)), stdout=f,
                       stderr=subprocess.STDOUT, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--suite", choices=["all", "large", "medium", "smoke"], default="all")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--include-1g", action="store_true")
    ap.add_argument("--skip-profiles", action="store_true",
                    help="Run uninstrumented timings only; diagnostics are enabled by default")
    args = ap.parse_args()
    if args.repeats < 1:
        ap.error("--repeats must be positive")
    limit = int(os.environ.get("LIMIT_SECONDS", "7200"))
    jobs = int(os.environ.get("BUILD_JOBS", "4"))
    if min(limit, jobs) < 1:
        ap.error("LIMIT_SECONDS and BUILD_JOBS must be positive")
    build = Path(os.environ.get("BUILD_DIR", ROOT / "build-linux")).resolve()
    results = Path(os.environ.get("RESULT_ROOT", ROOT / "server-results")).resolve()
    search = Path(os.environ.get("INDEX_SEARCH_ROOT", ROOT / "server-results")).resolve()
    data = Path(os.environ.get("DATA_ROOT", ROOT / "data")).resolve()
    results.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-isolated-"), dir=results))
    print("Results:", run, flush=True)
    rows, failed = [], False
    selected = {
        "all": ["dblp_apv_full", "dblp_v18", "imdb_legacy", "yelp"],
        "large": ["dblp_apv_full", "dblp_v18"],
        "medium": ["imdb_legacy", "yelp"],
        "smoke": ["dblp_small"],
    }[args.suite]
    configs = [(b, m) for m in ([32, 256, 1024] if args.include_1g else [32, 256])
               for b in ["pressure", "off", "always"]]
    modes = {"pressure": "6", "off": "7", "always": "8"}
    profile_groups = [0] if args.skip_profiles else [0, 1, 2]
    bin_dir = build / "bin"
    try:
        env = {"experiment_version": "isolated_lazy_v2",
               "activation": "lazy for all policies",
               "profile_note": "p0 is uninstrumented timing; p1/p2 diagnostic inclusive timings overlap and must not be summed",
               "platform": platform.platform(), "arguments": vars(args),
               "limit_seconds": limit, "index_search_root": str(search),
               "data_root": str(data), "build_dir": str(build)}
        for label, cmd in [("cpu", ["lscpu"]), ("memory", ["free", "-h"]),
                           ("compiler", ["g++", "--version"]),
                           ("commit", ["git", "-C", str(ROOT), "rev-parse", "HEAD"]),
                           ("git_diff", ["git", "-C", str(ROOT), "diff", "--", "CMakeLists.txt", "src"])]:
            p = subprocess.run(cmd, capture_output=True, text=True)
            env[label] = p.stdout + p.stderr
        (run / "environment.txt").write_text(json.dumps(env, indent=2), encoding="utf-8")
        checked(["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                 "-DCMAKE_CXX_COMPILER=g++"], run / "cmake-configure.log")
        targets = ["bri_build_index", "verify_adaptive_fli", "bri_query_witness_exclusion"]
        targets += [f"bri_isolated_{b}_{m}_p{p}" for b, m in configs for p in profile_groups]
        checked(["cmake", "--build", build, "--parallel", str(jobs), "--target", *targets],
                run / "cmake-build.log")
        with tempfile.TemporaryDirectory(prefix="oracle-", dir=run) as fixture:
            oracle = measured(run / "oracle", [bin_dir / "verify_adaptive_fli", fixture], limit)
        if oracle["status"] != 0 or oracle.get("all_passed") != "1":
            raise RuntimeError("Correctness oracle failed; see oracle.log")
        for name in selected:
            raw, path, eps = CASES[name]
            case_dir = run / name
            case_dir.mkdir()
            # Only recognized runner index paths; never use per-meta-path indexes.
            candidates = sorted(search.glob(f"*/indexes/{name}/base.bri")) if search.exists() else []
            index = candidates[-1] if candidates else case_dir / "base.bri"
            if not candidates:
                if not (data / raw / "base.txt").is_file():
                    raise RuntimeError(f"No reusable index or dataset for {name}")
                print(f"[{name}] building missing BRI (separate from online timings)", flush=True)
                metric = measured(case_dir / "offline-build",
                                  [bin_dir / "bri_build_index", data / raw, index], limit)
                (case_dir / "offline-build.json").write_text(json.dumps(metric, indent=2))
                if metric["status"] != 0:
                    raise RuntimeError(f"Index build failed: {name}")
            before = digest(index)
            (case_dir / "index.json").write_text(json.dumps(
                {"path": str(index), "sha256": before, "bytes": index.stat().st_size,
                 "reused": bool(candidates)}, indent=2))
            # Independent execution of unchanged production V14, not timed into ablations.
            print(f"[{name}] checking current production reference", flush=True)
            with tempfile.TemporaryDirectory(prefix="reference-", dir=case_dir) as output:
                reference = measured(case_dir / "reference",
                    [bin_dir / "bri_query_witness_exclusion", index, path, eps, "5", output], limit)
                if reference["status"] != 0:
                    raise RuntimeError(f"Reference failed: {name}")
                expected = {kind: digest(Path(output) / f"{kind}-{eps}-5.txt")
                            for kind in ["result", "roles"]}
            (case_dir / "reference-hashes.json").write_text(json.dumps(expected, indent=2))
            # All formal repeats first, then one diagnostic pass per profile group.
            passes = [(r, 0) for r in range(1, args.repeats + 1)]
            passes += [(1, p) for p in profile_groups if p]
            for repeat, profile_group in passes:
                order = configs[:]
                random.Random(20260908 + repeat).shuffle(order)
                for position, (bounds, mib) in enumerate(order, 1):
                    label = f"{bounds}-{mib}-p{profile_group}-r{repeat}"
                    print(f"[{name}] {label} ({position}/{len(order)})", flush=True)
                    with tempfile.TemporaryDirectory(prefix="output-", dir=case_dir) as output:
                        metrics = measured(case_dir / label,
                            [bin_dir / f"bri_isolated_{bounds}_{mib}_p{profile_group}", index, path, eps, "5", output], limit)
                        row = dict(dataset=name, meta_path=path, epsilon=eps, mu=5,
                                   repeat=repeat, order=position, bounds=bounds, cache_mib=mib,
                                   run_kind="diagnostic" if profile_group else "timing",
                                   expected_profile_group=profile_group, **metrics)
                        row["cluster_match"] = row["roles_match"] = "not_checked"
                        if metrics["status"] == 0:
                            for kind, key in [("result", "cluster_match"), ("roles", "roles_match")]:
                                file = Path(output) / f"{kind}-{eps}-5.txt"
                                row[key] = int(file.is_file() and digest(file) == expected[kind])
                        expected_mode = modes[bounds]
                        row["config_match"] = int(metrics.get("block_mode") == expected_mode and
                                                   metrics.get("cache_budget_mib") == str(mib) and
                                                   metrics.get("profile_group", "0") == str(profile_group))
                        row["policy_match"] = int(bounds != "off" or
                                                  metrics.get("witness_bound_checks") == "0")
                        row["valid"] = int(metrics["status"] == 0 and row["cluster_match"] == 1
                                           and row["roles_match"] == 1 and row["config_match"] == 1
                                           and row["policy_match"] == 1)
                        failed |= not row["valid"]
                        rows.append(row)
                        # Persist after every query, even if a subsequent query fails.
                        write_rows(run / "runs.tsv", [r for r in rows if r["run_kind"] == "timing"])
                        if profile_group:
                            write_rows(run / "diagnostics.tsv", [r for r in rows if r["run_kind"] == "diagnostic"])
            if digest(index) != before:
                raise RuntimeError(f"BRI changed during queries: {name}")
            (case_dir / "index-unchanged.txt").write_text("sha256_unchanged=1\n")
    except (Exception, KeyboardInterrupt) as exc:
        failed = True
        (run / "error.log").write_text(str(exc) + "\n")
        print("ERROR:", exc, file=sys.stderr)
    finally:
        summary = []
        for name in selected:
            for bounds, mib in configs:
                group = [r for r in rows if r["dataset"] == name and
                         r["bounds"] == bounds and r["cache_mib"] == mib and r["run_kind"] == "timing"]
                good = [r for r in group if r["valid"]]
                times = [r["elapsed_s"] for r in good]
                summary.append(dict(dataset=name, bounds=bounds, cache_mib=mib,
                    expected_n=args.repeats, attempted_n=len(group), valid_n=len(good),
                    complete=int(len(good) == args.repeats),
                    median_s=statistics.median(times) if times else "",
                    min_s=min(times) if times else "", max_s=max(times) if times else "",
                    peak_rss_kb=max(int(r.get("max_rss_kb", 0)) for r in good) if good else ""))
        write_rows(run / "summary.tsv", summary)
        (run / "status.txt").write_text("status=" + ("failed" if failed else "passed") + "\n")
        archive = run.with_suffix(".tar.gz")
        with tarfile.open(archive, "w:gz") as tar:
            for file in sorted(run.rglob("*")):
                if file.is_file() and file.suffix in [".log", ".time", ".tsv", ".txt", ".json"]:
                    tar.add(file, arcname=str(Path(run.name) / file.relative_to(run)))
        print("Send this archive:", archive, flush=True)
    return 1 if failed else 0


def write_rows(path, rows):
    columns = list(dict.fromkeys(key for row in rows for key in row))
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    sys.exit(main())
