#!/usr/bin/env python3
"""Tiny, independent semantic audit. Does not modify production algorithms.

Reference: HINSCAN Definitions 3.5--3.11 (non-independent model).
Paper mu counts self; upstream/current pSCAN mu counts OTHER similar neighbors.
An audit completing successfully does NOT mean production matches the paper.
Only generated fixtures in a new output directory are written.
"""
import argparse
from fractions import Fraction
import hashlib
import itertools
import json
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PAPER = "https://lai.me/files/HINSCAN-final.pdf"


def adjacency(n, edges):
    rows = [{u} for u in range(n)]
    for u, v in edges:
        if not (0 <= u < n and 0 <= v < n) or u == v:
            raise ValueError("fixture requires distinct, in-range endpoints")
        rows[u].add(v)
        rows[v].add(u)
    return rows


def reference(rows, epsilon, mu, *, include_self=True, legacy_roles=False):
    """Exhaustive sets/integers reference, independent of C++ factor/cache code."""
    e = Fraction(epsilon)
    if not 0 < e <= 1 or mu < (2 if include_self else 1):
        raise ValueError("invalid reference parameters")
    n = len(rows)
    for u, row in enumerate(rows):
        if u not in row or any(v < 0 or v >= n or u not in rows[v] for v in row):
            raise ValueError("reference requires symmetric closed neighborhoods")
    similar = []
    for u, row in enumerate(rows):
        similar.append({v for v in row if v != u and
                        len(row & rows[v]) ** 2 * e.denominator ** 2 >=
                        e.numerator ** 2 * len(row) * len(rows[v])})
    cores = [len(row) + int(include_self) >= mu for row in similar]
    memberships = [set() for _ in rows]
    for u in range(n):
        if not cores[u] or memberships[u]:
            continue
        # Increasing u makes the component id its smallest core id.
        queue = [u]
        memberships[u].add(u)
        for v in queue:
            for w in sorted(similar[v]):
                if cores[w] and not memberships[w]:
                    memberships[w].add(u)
                    queue.append(w)
    for u in range(n):
        if not cores[u]:
            for v in similar[u]:
                if cores[v]:
                    memberships[u].update(memberships[v])
    roles = []
    for u in range(n):
        if cores[u]:
            role = "core"
        elif memberships[u]:
            # A noncore can belong to multiple clusters without being a hub.
            role = "hub" if legacy_roles and len(memberships[u]) > 1 else "border"
        elif legacy_roles:
            role = "outlier"
        else:
            # Include clustered NONCORE neighbors, not only core neighbors.
            adjacent_clusters = set()
            for v in rows[u] - {u}:
                adjacent_clusters.update(memberships[v])
            role = "hub" if len(adjacent_clusters) >= 2 else "outlier"
        roles.append(role)
    return dict(cores=cores, memberships=[sorted(x) for x in memberships], roles=roles)


def clique(vertices):
    return list(itertools.combinations(vertices, 2))


def fixtures():
    return [
        dict(name="triangle_mu_boundary", n=3, edges=clique(range(3)), epsilon="1", mu=3),
        dict(name="unassigned_hub", n=10,
             edges=clique(range(4)) + clique(range(4, 8)) + [(8, 0), (8, 4)],
             epsilon="0.8", mu=4),
        dict(name="overlapping_border", n=11,
             edges=clique(range(5)) + clique(range(5, 10)) + [(10, 0), (10, 5)],
             epsilon="0.45", mu=5),
        dict(name="hub_via_border_neighbors", n=14,
             edges=clique(range(5)) + clique(range(5, 10)) +
             [(10, 0), (11, 5), (12, 10), (12, 11)], epsilon="0.45", mu=5),
        dict(name="single_border", n=7,
             edges=clique(range(5)) + [(5, 0)], epsilon="0.5", mu=5),
        dict(name="isolates", n=3, edges=[], epsilon="1", mu=2),
        dict(name="long_path_dedup", n=5, edges=[(0, 1), (1, 2), (2, 3)],
             epsilon="1", mu=2, path="A-B-A-B-A", duplicate=True),
        dict(name="mu_above_degree", n=3, edges=clique(range(3)), epsilon="1", mu=5),
    ]


def incidence(n, edges, duplicate=False):
    """One B witness per undirected edge: ABA projects to exactly that graph."""
    pairs = [(u, i) for i, edge in enumerate(edges) for u in edge]
    if duplicate and pairs:
        pairs += [pairs[0], pairs[0]]
    return [n, len(edges)], [(0, 1, pairs)]


def full_path_rows(counts, relations, path):
    types = path.split("-")
    ids = [ord(x) - ord("A") for x in types]
    transitions = {}
    for a, b, pairs in relations:
        if (a, b) in transitions or (b, a) in transitions:
            raise ValueError("ambiguous type-only path")
        forward = [set() for _ in range(counts[a])]
        reverse = [set() for _ in range(counts[b])]
        for u, v in pairs:
            forward[u].add(v)
            reverse[v].add(u)
        transitions[a, b] = forward
        transitions[b, a] = reverse
    result = []
    for u in range(counts[ids[0]]):
        frontier = {u}
        for a, b in zip(ids, ids[1:]):
            next_frontier = set()
            for v in frontier:
                next_frontier.update(transitions[a, b][v])
            frontier = next_frontier
        result.append(frontier | {u})
    return result


def write_hin(directory, counts, relations):
    (directory / "edge").mkdir(parents=True)
    base = [str(len(counts))] + [f"{chr(65+i)} {n}" for i, n in enumerate(counts)]
    base.append(str(len(relations)))
    for i, (a, b, pairs) in enumerate(relations):
        header = f"{a} {b} {len(pairs)}"
        base.append(header)
        (directory / "edge" / f"{i}.txt").write_text(
            header + "\n" + "".join(f"{u} {v}\n" for u, v in pairs), encoding="utf-8")
    (directory / "base.txt").write_text("\n".join(base) + "\n", encoding="utf-8")


def write_binary(directory, rows):
    directory.mkdir()
    neighbors = [sorted(row - {u}) for u, row in enumerate(rows)]
    values = [4, len(rows), sum(map(len, neighbors))] + list(map(len, neighbors))
    (directory / "b_degree.bin").write_bytes(struct.pack(f"<{len(values)}I", *values))
    flat = [v for row in neighbors for v in row]
    (directory / "b_adj.bin").write_bytes(struct.pack(f"<{len(flat)}I", *flat))


def read_binary(directory):
    data = (directory / "b_degree.bin").read_bytes()
    if len(data) % 4:
        raise ValueError("truncated degree file")
    values = struct.unpack(f"<{len(data)//4}I", data)
    if len(values) < 3 or values[0] != 4 or len(values) != values[1] + 3:
        raise ValueError("invalid binary degree header")
    n, m = values[1:3]
    data = (directory / "b_adj.bin").read_bytes()
    if len(data) != 4 * m or sum(values[3:]) != m:
        raise ValueError("invalid binary adjacency length")
    flat = struct.unpack(f"<{m}I", data)
    rows, pos = [], 0
    for u, degree in enumerate(values[3:]):
        row = list(flat[pos:pos+degree])
        if row != sorted(set(row)) or u in row or any(v >= n for v in row):
            raise ValueError("invalid binary adjacency row")
        rows.append(set(row) | {u})
        pos += degree
    return rows


def read_result(path, n):
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0].strip() != "c/n vertex_id cluster_id":
        raise ValueError("bad result header")
    cores, memberships, seen = [False] * n, [set() for _ in range(n)], set()
    for line in lines[1:]:
        kind, u, c = line.split()
        u, c = int(u), int(c)
        if kind not in ("c", "n") or not 0 <= u < n or not 0 <= c < n:
            raise ValueError("bad result record")
        if (u, c) in seen or (memberships[u] and (cores[u] or kind == "c")):
            raise ValueError("duplicate or mixed core/noncore record")
        seen.add((u, c))
        cores[u] = kind == "c"
        memberships[u].add(c)
    return dict(cores=cores, memberships=[sorted(x) for x in memberships])


def read_roles(path, n):
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0].strip() != "vertex_id role cluster_count clusters":
        raise ValueError("bad roles header")
    roles, memberships = [None] * n, [None] * n
    for line in lines[1:]:
        fields = line.split()
        u, role, size = int(fields[0]), fields[1], int(fields[2])
        ids = list(map(int, fields[3:]))
        if not 0 <= u < n or roles[u] is not None or role not in ("core", "border", "hub", "outlier"):
            raise ValueError("bad role record")
        if size != len(ids) or ids != sorted(set(ids)) or any(c < 0 or c >= n for c in ids):
            raise ValueError("bad role memberships")
        roles[u], memberships[u] = role, ids
    if any(x is None for x in roles):
        raise ValueError("missing role vertex")
    return dict(roles=roles, memberships=memberships)


def cluster_part(result):
    return {key: result[key] for key in ("cores", "memberships")}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(command, log, *, reject_message=None):
    run = subprocess.run([str(x) for x in command], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", timeout=60)
    text = run.stdout + run.stderr
    log.write_text(text, encoding="utf-8")
    if reject_message is None:
        if run.returncode:
            raise RuntimeError(f"command failed ({run.returncode}): {log}")
    elif run.returncode == 0 or reject_message not in text:
        raise RuntimeError(f"expected explicit rejection containing {reject_message!r}: {log}")
    return dict(command=list(map(str, command)), exit_code=run.returncode, log=log.name)


def audit(bin_dir, output):
    output.mkdir(parents=True, exist_ok=False)
    report = dict(audit_completed=False, paper_semantics_verified=False,
                  production_modified=False, timing_benchmark=False, source=PAPER,
                  reference_scope="Formal non-independent HINSCAN definitions; symmetric fixture paths only.",
                  mu_mapping="paper_mu = upstream_other_neighbor_mu + 1 (paper_mu >= 2)",
                  cases=[], scope_checks=[], unexpected_failures=[])
    commands = []

    def run(command, log, **kw):
        commands.append(execute(command, log, **kw))

    def check(ok, message):
        if not ok:
            raise AssertionError(message)

    try:
        names = ["pscan_baseline", "hin_materialize", "bri_build_index",
                 "bri_query_core_connectivity", "bri_query_core_lean"]
        binaries = {}
        for name in names:
            path = bin_dir / name
            if not path.is_file():
                path = bin_dir / (name + ".exe")
            if not path.is_file():
                raise FileNotFoundError(f"build required target: {name}")
            binaries[name] = path.resolve()
        report["binaries_sha256"] = {k: sha256(v) for k, v in binaries.items()}
        sources = ["third_party/pscan/Graph.cpp", "src/scan/PscanOnFli.cpp",
                   "src/tools/verify_anchor_filter.cpp", "src/tools/verify_adaptive_fli.cpp",
                   "src/hin/HinGraph.cpp", "src/index/FactorIndex.cpp",
                   "experiments/audit_semantics.py"]
        report["sources_sha256"] = {p: sha256(ROOT / p) for p in sources}
        for fixture in fixtures():
            case = output / fixture["name"]
            case.mkdir()
            path = fixture.get("path", "A-B-A")
            counts, relations = incidence(fixture["n"], fixture["edges"], fixture.get("duplicate", False))
            rows = full_path_rows(counts, relations, path)
            eps, paper_mu = fixture["epsilon"], fixture["mu"]
            paper = reference(rows, eps, paper_mu)
            current_same = reference(rows, eps, paper_mu, include_self=False, legacy_roles=True)
            normalized_legacy = reference(rows, eps, paper_mu-1, include_self=False, legacy_roles=True)
            check(cluster_part(paper) == cluster_part(normalized_legacy), "mu mapping inconsistent")
            entry = dict(name=fixture["name"], n=len(rows), epsilon=eps, paper_mu=paper_mu,
                         path=path, edges=fixture["edges"], closed_rows=[sorted(x) for x in rows],
                         paper_reference=paper, upstream=[], indexed=[])
            report["cases"].append(entry)
            hin = case / "hin"
            write_hin(hin, counts, relations)
            direct = case / "direct-binary"
            write_binary(direct, rows)
            projected = case / "cpp-projection"
            run([binaries["hin_materialize"], hin, path, projected], case / "materialize.log")
            check(read_binary(projected) == rows, "C++ projection differs from independent traversal")
            entry["projection_matches_independent_traversal"] = True
            index = case / "base.bri"
            run([binaries["bri_build_index"], hin, index], case / "build-index.log")
            for mu, label, expected in [(paper_mu, "same-numeric-mu", current_same),
                                        (paper_mu-1, "normalized-mu", normalized_legacy)]:
                run([binaries["pscan_baseline"], direct, eps, mu, "output"], case / f"upstream-{label}.log")
                got = read_result(direct / f"result-{eps}-{mu}.txt", len(rows))
                check(got == cluster_part(expected), f"upstream unexpected clusters: {fixture['name']} {label}")
                entry["upstream"].append(dict(mu=mu, label=label, result=got,
                    clusters_match_paper=(got == cluster_part(paper)), roles_available=False))
                for name in ("bri_query_core_connectivity", "bri_query_core_lean"):
                    target = case / f"{name}-{label}"
                    run([binaries[name], index, path, eps, mu, target], case / f"{name}-{label}.log")
                    result = read_result(target / f"result-{eps}-{mu}.txt", len(rows))
                    role_data = read_roles(target / f"roles-{eps}-{mu}.txt", len(rows))
                    check(result == cluster_part(expected), f"indexed unexpected clusters: {fixture['name']} {name} {label}")
                    check(role_data["memberships"] == result["memberships"], "roles/result membership mismatch")
                    check(role_data["roles"] == expected["roles"], "indexed unexpected legacy roles")
                    entry["indexed"].append(dict(executable=name, mu=mu, label=label,
                        result=result, roles=role_data["roles"], matches_legacy_reference=True,
                        clusters_match_paper=(result == cluster_part(paper)),
                        roles_match_paper=(role_data["roles"] == paper["roles"]),
                        role_difference_vertices=[u for u in range(len(rows)) if role_data["roles"][u] != paper["roles"][u]]))
        # Record rejection, not a fabricated reference answer, for unsupported paths.
        scope_fixtures = [
            ("non_symmetric_closed_path", [2, 1, 1],
             [(0, 1, [(0, 0)]), (1, 2, [(0, 0)]), (2, 0, [(0, 1)])],
             "A-B-C-A", "symmetric"),
            ("ambiguous_relation_types", [2, 2],
             [(0, 1, [(0, 0)]), (0, 1, [(1, 1)])], "A-B-A", "ambiguous"),
        ]
        for name, counts, relations, path, message in scope_fixtures:
            case = output / name
            case.mkdir()
            write_hin(case / "hin", counts, relations)
            index = case / "base.bri"
            run([binaries["bri_build_index"], case / "hin", index], case / "build.log")
            run([binaries["bri_query_core_lean"], index, path, "0.8", 3, case / "out"],
                case / "query.log", reject_message=message)
            run([binaries["hin_materialize"], case / "hin", path, case / "projected"],
                case / "materialize.log", reject_message=message)
            report["scope_checks"].append(dict(name=name, path=path, rejected=True,
                expected_diagnostic=message, rejection_does_not_define_the_paper_problem=True))
        check(any(not x["clusters_match_paper"] for c in report["cases"] for x in c["upstream"]),
              "mu counterexample not reproduced")
        check(any(not x["roles_match_paper"] for c in report["cases"] for x in c["indexed"]
                  if x["label"] == "normalized-mu"), "role counterexample not reproduced")
        report["audit_completed"] = True
        report["status"] = "known_semantic_differences_reproduced"
    except Exception as error:
        report["unexpected_failures"].append(f"{type(error).__name__}: {error}")
        report["status"] = "audit_execution_failed"
    report["commands"] = commands
    (output / "audit.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=ROOT / "build-ablation-local" / "bin")
    parser.add_argument("--output", type=Path, help="New directory; existing paths are refused")
    args = parser.parse_args()
    if args.output is None:
        # Own a new parent; audit itself still refuses an existing result directory.
        args.output = Path(tempfile.mkdtemp(prefix="hinscan-semantics-")) / "audit"
    report = audit(args.bin_dir.resolve(), args.output.resolve())
    print(f"Report: {args.output.resolve() / 'audit.json'}")
    print(f"audit_completed={int(report['audit_completed'])}")
    print("paper_semantics_verified=0")
    print(f"status={report['status']}")
    for failure in report["unexpected_failures"]:
        print(failure)
    return 0 if report["audit_completed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
