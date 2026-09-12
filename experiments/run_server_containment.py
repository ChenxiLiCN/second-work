#!/usr/bin/env python3
"""Linux-only, one-run graph/schema containment structure diagnostic."""
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
DATASETS = ("yelp", "imdb_large", "foursquare")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024*1024), b""):
            h.update(block)
    return h.hexdigest()


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def input_files(directory):
    from prepare_selected_data import schema
    _, relations = schema(directory / "base.txt")
    paths = [directory / "base.txt"] + [directory / "edge" / f"{i}.txt" for i in range(len(relations))]
    if not all(p.is_file() for p in paths):
        raise ValueError(f"Incomplete normalized HIN directory: {directory}")
    return paths


def find_normalized(name, data, results):
    candidates = [data / "derived/normalized" / name]
    # Reuse only adapter outputs with a matching dataset audit, not loose files.
    for run in sorted((p for p in results.iterdir() if p.is_dir()), reverse=True):
        audit = run / name / "data-audit.json"
        try:
            info = json.loads(audit.read_text(encoding="utf-8"))
            if info.get("dataset") == name:
                candidates.append(run / name / "normalized")
        except (OSError, ValueError):
            continue
    if name == "yelp":
        candidates.append(data / "raw/hin_text/yelp")
    for path in candidates:
        try:
            input_files(path)
            return path.resolve()
        except (OSError, ValueError, IndexError):
            continue
    return None


def snapshot(directory):
    return {str(p.relative_to(directory)): dict(bytes=p.stat().st_size, sha256=digest(p))
            for p in input_files(directory)}


def validate_structure(output, report):
    if (report.get("complete") is not True or report.get("diagnostic_only") is not True
            or report.get("query_inputs_used") is not False
            or report.get("online_speedup_measured") is not False):
        raise ValueError("Incomplete or wrong-scope diagnostic report")
    total = 0
    records = []
    for view in report["views"]:
        name = f"relation-{view['relation']}-{view['direction']}.rcf"
        if view["file"] != name:
            raise ValueError("Unexpected forest filename")
        path = output / name
        with path.open("rb") as f:
            head = f.read(56)
        if len(head) != 56 or head[:8] != b"RCFORE01":
            raise ValueError("Invalid forest header")
        r, d, source, target, n, m, k, q = struct.unpack_from("<4I4Q", head, 8)
        if ((r, d, source, target, n, k, q) !=
                (view["relation"], view["direction"], view["source_type"], view["target_type"],
                 view["source_vertices"], view["nonempty_rows"], view["classes"])):
            raise ValueError("Forest header/report mismatch")
        actual = path.stat().st_size
        if actual != 56+8*k+24*q or actual != view["forest_bytes"]:
            raise ValueError("Forest size mismatch")
        if not 0 <= view["parent_edges"] <= max(0, q-1):
            raise ValueError("Invalid forest edge count")
        total += actual
        records.append(dict(file=name, bytes=actual, sha256=digest(path)))
    if total != report["forest_bytes"]:
        raise ValueError("Total forest size mismatch")
    return records


def checked(command, log, env=None):
    with log.open("w", encoding="utf-8") as f:
        subprocess.run(list(map(str, command)), stdout=f, stderr=subprocess.STDOUT,
                       check=True, cwd=ROOT, env=env)


def main():
    if platform.system() != "Linux":
        raise SystemExit("Server-only script: run on Linux. Do not run real datasets on the local PC.")
    data = Path(os.environ.get("DATA_ROOT", ROOT / "data")).resolve()
    results = Path(os.environ.get("RESULT_ROOT", ROOT / "server-results")).resolve()
    build = Path(os.environ.get("BUILD_DIR", ROOT / "build-linux-containment")).resolve()
    jobs = int(os.environ.get("BUILD_JOBS", "2"))
    limit = int(os.environ.get("LIMIT_SECONDS", "7200"))
    if min(jobs, limit) < 1:
        raise ValueError("BUILD_JOBS and LIMIT_SECONDS must be positive")
    results.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=time.strftime("%Y%m%d-%H%M%S-containment-"), dir=results))
    print("Results:", run, flush=True)
    status = dict(all_completed=False, diagnostic_only=True, datasets={}, errors=[])
    try:
        env = dict(version="containment_structure_v1", repeats=1, datasets=list(DATASETS),
                   platform=platform.platform(), data_root=str(data), build_dir=str(build),
                   limit_seconds=limit, no_queries=True, online_speedup_measured=False,
                   size_scope="Forest sidecars ONLY; base graph excluded. Ratio uses normalized text bytes.",
                   timing_scope="Offline compute excludes raw input parsing and output writes. Peak RSS includes loaded graph and temporary build workspaces.",
                   source_sha256={p: digest(ROOT/p) for p in [
                       "CMakeLists.txt", "src/hin/HinGraph.cpp",
                       "src/index/RelationContainmentForest.h", "src/index/RelationContainmentForest.cpp",
                       "src/tools/diagnose_containment.cpp", "experiments/test_containment_forest.py",
                       "experiments/run_server_containment.py", "experiments/prepare_selected_data.py"]})
        save_json(run / "environment.json", env)
        checked(["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                 "-DBUILD_PSCAN_BASELINE=OFF"], run / "configure.log")
        checked(["cmake", "--build", build, "--parallel", jobs, "--target", "diagnose_containment"],
                run / "build.log")
        binary = build / "bin/diagnose_containment"
        test_env = os.environ.copy()
        test_env["CONTAINMENT_BIN"] = str(binary)
        checked([sys.executable, "-B", "-m", "unittest", "discover", "-s", ROOT / "experiments",
                 "-p", "test_containment*.py", "-v"], run / "tests.log", test_env)
        env["binary_sha256"] = digest(binary)
        save_json(run / "environment.json", env)
        for name in DATASETS:
            case = run / name
            case.mkdir()
            try:
                normalized = find_normalized(name, data, results)
                reused = normalized is not None
                if normalized is None:
                    from prepare_selected_data import prepare
                    normalized = case / "normalized"
                    print(f"[{name}] preparing lossless text format (outside index timing)", flush=True)
                    save_json(case / "data-audit.json", prepare(name, data, normalized))
                before = snapshot(normalized)
                save_json(case / "input.json", dict(path=str(normalized), reused=reused, files=before))
                print(f"[{name}] graph/schema-only forest build (once)", flush=True)
                with (case / "diagnostic.log").open("w", encoding="utf-8") as log:
                    result = subprocess.run(
                        ["/usr/bin/time", "-f", "max_rss_kb=%M", "-o", str(case / "memory.log"),
                         "timeout", "--kill-after=30s", str(limit)+"s", str(binary),
                         str(normalized), str(case / "forest")],
                        stdout=log, stderr=subprocess.STDOUT)
                if result.returncode:
                    raise RuntimeError(f"Diagnostic failed or timed out (exit {result.returncode}); no complete result")
                report = json.loads((case / "forest/structure.json").read_text(encoding="utf-8"))
                records = validate_structure(case / "forest", report)
                if snapshot(normalized) != before:
                    raise RuntimeError("Source files changed during diagnostic")
                input_bytes = sum(v["bytes"] for v in before.values())
                summary = dict(**report, input_normalized_text_bytes=input_bytes,
                               additional_forest_to_text_ratio=report["forest_bytes"]/input_bytes if input_bytes else None,
                               forest_manifest=records,
                               memory=(case / "memory.log").read_text(),
                               base_graph_included_in_forest_bytes=False)
                save_json(case / "summary.json", summary)
                status["datasets"][name] = dict(complete=True, forest_bytes=report["forest_bytes"],
                    offline_compute_ms=report["offline_compute_ms"])
            except Exception as error:
                status["errors"].append(f"{name}: {type(error).__name__}: {error}")
                status["datasets"][name] = dict(complete=False)
                print(status["errors"][-1], flush=True)
        status["all_completed"] = not status["errors"] and len(status["datasets"]) == len(DATASETS)
    except Exception as error:
        status["errors"].append(f"{type(error).__name__}: {error}")
    save_json(run / "status.json", status)
    archive = run.with_suffix(".tar.gz")
    # Ship reports/logs only; keep data and forest sidecars on the server.
    with tarfile.open(archive, "w:gz") as tar:
        for path in sorted(run.rglob("*")):
            relative = path.relative_to(run)
            if path.is_file() and "normalized" not in relative.parts and path.suffix in (".json", ".log"):
                tar.add(path, arcname=str(Path(run.name) / relative), recursive=False)
    print("Send this archive:", archive, flush=True)
    return 0 if status["all_completed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
