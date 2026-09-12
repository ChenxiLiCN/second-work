#!/usr/bin/env python3
"""Passing regression for repaired BRI HINSCAN semantics; tiny fixtures only."""
import argparse
import itertools
import json
from pathlib import Path
import tempfile

from audit_semantics import (ROOT, PAPER, cluster_part, execute, fixtures, full_path_rows,
                             incidence, read_binary, read_result, read_roles, reference,
                             sha256, write_binary, write_hin)

QUERY_NAMES = ("bri_query_index", "bri_query_witness_exclusion",
               "bri_query_core_connectivity", "bri_query_core_lean", "bri_query_regions")


def metrics(log):
    return dict(line.split("=", 1) for line in log.read_text(encoding="utf-8").splitlines()
                if "=" in line)


def verify(bin_dir, output):
    output.mkdir(parents=True, exist_ok=False)
    report = dict(status="running", all_passed=False, source=PAPER, timing_benchmark=False,
                  scope="Non-independent model, tiny symmetric paths; not proof for all inputs.",
                  cases=[], rejections=[], errors=[])
    try:
        binaries = {}
        for name in (*QUERY_NAMES, "pscan_baseline", "hin_materialize", "bri_build_index"):
            path = bin_dir / name
            if not path.is_file():
                path = bin_dir / (name + ".exe")
            if not path.is_file():
                raise FileNotFoundError(f"Build target required: {name}")
            binaries[name] = path.resolve()
        report["binary_sha256"] = {name: sha256(path) for name, path in binaries.items()}
        report["source_sha256"] = {
            str(path.relative_to(ROOT)): sha256(path)
            for path in [ROOT / "src/scan/PscanOnFli.cpp", ROOT / "src/scan/PscanOnFli.h",
                         ROOT / "src/tools/bri_query_index.cpp",
                         ROOT / "src/tools/bri_query_regions.cpp",
                         ROOT / "experiments/audit_semantics.py", Path(__file__).resolve()]}
        cases = fixtures()
        cases.append(dict(name="huge_mu_no_core", n=3,
                          edges=list(itertools.combinations(range(3), 2)),
                          epsilon="1", mu=2**64-1))
        # Exhaustive graph structures; rotate parameters, not an exhaustive
        # epsilon/mu sweep. Each variant runs once per selected configuration.
        edges = list(itertools.combinations(range(4), 2))
        for mask in range(64):
            cases.append(dict(name=f"four_vertex_{mask:02d}", n=4,
                              edges=[edge for i, edge in enumerate(edges) if mask & (1 << i)],
                              epsilon=("0.5", "0.8", "1")[mask % 3],
                              mu=(2, 3, 4, 6)[mask % 4]))
        for c in cases:
            case = output / c["name"]
            case.mkdir()
            path, eps, mu = c.get("path", "A-B-A"), c["epsilon"], c["mu"]
            counts, relations = incidence(c["n"], c["edges"], c.get("duplicate", False))
            rows = full_path_rows(counts, relations, path)
            truth = reference(rows, eps, mu)
            write_hin(case / "hin", counts, relations)
            index = case / "base.bri"
            execute([binaries["bri_build_index"], case / "hin", index], case / "build.log")
            execute([binaries["hin_materialize"], case / "hin", path, case / "projection"],
                    case / "projection.log")
            assert read_binary(case / "projection") == rows, "projection differs from full traversal"
            direct = case / "direct"
            write_binary(direct, rows)
            entry = dict(name=c["name"], epsilon=eps, paper_mu=mu, path=path,
                         paper_reference=truth, variants=[], upstream_cluster_check=None)
            report["cases"].append(entry)
            # Upstream atoi(int) cannot represent the deliberate uint64 boundary.
            if mu <= 2**31-1:
                execute([binaries["pscan_baseline"], direct, eps, mu-1, "output"], case / "upstream.log")
                got = read_result(direct / f"result-{eps}-{mu-1}.txt", len(rows))
                assert got == cluster_part(truth), "mapped upstream cluster mismatch"
                entry["upstream_cluster_check"] = True
            for name in QUERY_NAMES:
                dest = case / name
                log = case / (name + ".log")
                execute([binaries[name], index, path, eps, mu, dest], log)
                got = read_result(dest / f"result-{eps}-{mu}.txt", len(rows))
                roles = read_roles(dest / f"roles-{eps}-{mu}.txt", len(rows))
                assert got == cluster_part(truth), f"{c['name']} {name}: cluster mismatch"
                assert roles["memberships"] == truth["memberships"], f"{name}: role memberships"
                assert roles["roles"] == truth["roles"], f"{c['name']} {name}: role mismatch"
                info = metrics(log)
                assert info["semantics_version"] == "hinscan_nonindependent_v1"
                assert info["mu_counts_self"] == "1"
                assert int(info["mu"]) == mu and int(info["pscan_other_mu"]) == mu-1
                assert float(info["online_compute_ms"]) >= 0 and float(info["role_ms"]) >= 0
                if "noncore_ms" in info:
                    assert float(info["role_ms"]) <= float(info["noncore_ms"]) + 0.001
                entry["variants"].append(dict(name=name, clusters=True, roles=True, metadata=True))
            print(f"[{len(report['cases'])}/{len(cases)}] {c['name']}: all five variants match", flush=True)

        # Invalid mu is rejected before the index is opened. No extra query knobs.
        for name in QUERY_NAMES:
            for mu in ("0", "1", "-1"):
                log = output / f"{name}-invalid-{mu}.log"
                execute([binaries[name], output / "nonexistent.bri", "A-B-A", "0.5", mu,
                         output / "must-not-exist"], log, reject_message="mu")
                assert not (output / "must-not-exist").exists()
                report["rejections"].append(dict(executable=name, mu=mu, rejected=True))
        report["all_passed"] = True
        report["status"] = "passed_within_tested_scope"
    except Exception as error:
        report["errors"].append(f"{type(error).__name__}: {error}")
        report["status"] = "failed"
    (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=ROOT / "build-ablation-local/bin")
    parser.add_argument("--output", type=Path, help="New directory; existing output is refused")
    args = parser.parse_args()
    if args.output is None:
        args.output = Path(tempfile.mkdtemp(prefix="hinscan-correctness-")) / "verification"
    r = verify(args.bin_dir.resolve(), args.output.resolve())
    print(f"Report: {args.output.resolve() / 'verification.json'}")
    print(f"all_passed={int(r['all_passed'])}")
    for error in r["errors"]:
        print(error)
    return 0 if r["all_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

