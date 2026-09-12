"""Independent finite-model checks and tiny C++ forest serialization tests."""
from fractions import Fraction
import itertools
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

from audit_semantics import ROOT, execute, reference, write_hin


def forest_model(rows):
    classes = sorted({tuple(sorted(row)) for row in rows if row})
    mapping = [classes.index(tuple(sorted(row))) if row else None for row in rows]
    parent = []
    for row in classes:
        candidates = [i for i, other in enumerate(classes) if set(row) < set(other)]
        parent.append(min(candidates, key=lambda i: (len(classes[i]), i)) if candidates else None)
    return classes, mapping, parent


def ancestor(parent, a, b):
    while b is not None:
        if a == b:
            return True
        b = parent[b]
    return False


def comparable(parent, a, b):
    return a is not None and b is not None and (ancestor(parent, a, b) or ancestor(parent, b, a))


def half_rows(rows, suffix):
    return [set().union(*(suffix[x] for x in row)) if row else set() for row in rows]


def closed_rows(half):
    return [{u} | {v for v in range(len(half)) if half[u] & half[v]} for u in range(len(half))]


def components(vertices, edges):
    neighbors = {u: set() for u in vertices}
    for u, v in edges:
        neighbors[u].add(v)
        neighbors[v].add(u)
    groups = []
    while neighbors:
        queue = [next(iter(neighbors))]
        group = set()
        for u in queue:
            if u not in neighbors:
                continue
            group.add(u)
            queue.extend(neighbors.pop(u))
        groups.append(tuple(sorted(group)))
    return sorted(groups)


def check_theorems(test, rows, suffix):
    classes, mapping, parent = forest_model(rows)
    half = half_rows(rows, suffix)
    closed = closed_rows(half)
    n = len(rows)
    for u, v in itertools.permutations(range(n), 2):
        if rows[u] <= rows[v]:
            test.assertTrue(half[u] <= half[v])
        if half[u] and half[v] and comparable(parent, mapping[u], mapping[v]):
            test.assertIn(v, closed[u])
            test.assertEqual(len(closed[u] & closed[v]), min(len(closed[u]), len(closed[v])))
    for epsilon in ("0.5", "0.8", "1"):
        e = Fraction(epsilon)
        def similar(u, v):
            return (v in closed[u] and len(closed[u] & closed[v])**2 * e.denominator**2
                    >= e.numerator**2 * len(closed[u]) * len(closed[v]))
        def eligible(u, v):
            return (bool(half[u]) and bool(half[v]) and
                    comparable(parent, mapping[u], mapping[v]))
        for u in range(n):
            certified_count = 1
            for c in range(len(classes)):
                members = [v for v in range(n) if mapping[v] == c]
                v = members[0]
                if not eligible(u, v):
                    continue
                du, dv = len(closed[u]), len(closed[v])
                decision = min(du, dv) * e.denominator**2 >= max(du, dv) * e.numerator**2
                test.assertEqual(decision, similar(u, v))
                if decision:
                    certified_count += len(members) - int(u in members)
            true_count = sum(similar(u, v) for v in range(n))
            test.assertLessEqual(certified_count, true_count)
            for mu in (2, 3, 5):
                if certified_count >= mu:
                    test.assertTrue(reference(closed, epsilon, mu)["cores"][u])
        # A stronger connectivity check: arbitrary active class-core masks.
        active_classes = {mapping[u] for u in range(n) if half[u]}
        for bits in range(1 << len(classes)):
            core_classes = {c for c in active_classes if bits & (1 << c)}
            full = []
            nearest = []
            reps = {c: mapping.index(c) for c in core_classes}
            for a, b in itertools.combinations(core_classes, 2):
                if comparable(parent, a, b) and similar(reps[a], reps[b]):
                    full.append((a, b))
            for c in core_classes:
                p = parent[c]
                while p is not None and p not in core_classes:
                    p = parent[p]
                if p is not None and similar(reps[c], reps[p]):
                    nearest.append((c, p))
            test.assertEqual(components(core_classes, full), components(core_classes, nearest))


def read_forest(path):
    data = path.read_bytes()
    if len(data) < 56 or data[:8] != b"RCFORE01":
        raise ValueError("invalid forest header")
    relation, direction, source, target, n, m, k, q = struct.unpack_from("<4I4Q", data, 8)
    if len(data) != 56 + 8*k + 24*q:
        raise ValueError("invalid forest length")
    membership = [struct.unpack_from("<2I", data, 56+8*i) for i in range(k)]
    nodes = [struct.unpack_from("<6I", data, 56+8*k+24*i) for i in range(q)]
    return dict(relation=relation, direction=direction, source=source, target=target,
                n=n, m=m, membership=membership, nodes=nodes)


class ContainmentTheoryTests(unittest.TestCase):
    def test_exhaustive_small_relations_and_suffixes(self):
        # 512 source relations, four suffix shapes including complete inactivity.
        subsets = [set(i for i in range(3) if bits & (1 << i)) for bits in range(8)]
        suffixes = [[{0}, {1}, {2}], [set(), set(), set()],
                    [{0}, {0}, {0}], [{0, 1}, set(), {1}]]
        for rows in itertools.product(subsets, repeat=3):
            for suffix in suffixes:
                check_theorems(self, rows, suffix)

    def test_chain_equal_sets_and_core_ancestor_gaps(self):
        rows = [{0}, {0}, {0, 1}, {0, 1, 2}, {0, 1, 2, 3}, set()]
        check_theorems(self, rows, [{0}, {1}, {2}, {3}])

    def test_diamond_loses_true_containment(self):
        rows = [{0}, {0, 1}, {0, 2}]
        _, mapping, parent = forest_model(rows)
        self.assertTrue(rows[0] < rows[2])
        self.assertFalse(comparable(parent, mapping[0], mapping[2]))
        check_theorems(self, rows, [{0}, {1}, {2}])

    def test_inactive_equal_rows_are_not_a_clique(self):
        rows = [{0}, {0}, {1}]
        half = half_rows(rows, [set(), {0}])
        self.assertEqual(closed_rows(half), [{0}, {1}, {2}])
        self.assertFalse(any(reference(closed_rows(half), "1", 2)["cores"]))

    def test_cross_branch_fallback_is_necessary(self):
        rows = [{0, 1}, {1, 2}]
        _, mapping, parent = forest_model(rows)
        self.assertFalse(comparable(parent, *mapping))
        result = reference(closed_rows(rows), "1", 2)
        self.assertEqual(result["cores"], [True, True])
        self.assertEqual(result["memberships"], [[0], [0]])


class ContainmentCppTests(unittest.TestCase):
    def test_exact_build_serialization_and_metrics(self):
        binary = Path(os.environ.get("CONTAINMENT_BIN", ROOT / "build-ablation-local/bin/diagnose_containment"))
        if not binary.is_file():
            binary = binary.with_suffix(".exe")
        self.assertTrue(binary.is_file(), "Build diagnose_containment before running this test")
        binary = binary.resolve()
        relations = []
        # All 3-by-2 Boolean relations, plus a chain and a diamond.
        for bits in range(64):
            relations.append(([3, 2], [(u, v) for u in range(3) for v in range(2)
                                      if bits & (1 << (2*u+v))]))
        relations += [([5, 4], [(u, v) for u in range(5) for v in range(min(u+1, 4))]),
                      ([3, 3], [(0, 0), (1, 0), (1, 1), (2, 0), (2, 2)]),
                      ([3, 0], [])]
        with tempfile.TemporaryDirectory(prefix="containment-tiny-") as tmp:
            root = Path(tmp)
            for i, (counts, pairs) in enumerate(relations):
                case = root / str(i)
                case.mkdir()
                write_hin(case / "hin", counts, [(0, 1, pairs + pairs[:1])])
                execute([binary, case / "hin", case / "out"], case / "run.log")
                report = json.loads((case / "out/structure.json").read_text())
                self.assertTrue(report["complete"])
                self.assertFalse(report["query_inputs_used"])
                actual_total = 0
                for direction in (0, 1):
                    n, m = counts if direction == 0 else counts[::-1]
                    rows = [set() for _ in range(n)]
                    for a, b in pairs:
                        u, v = (a, b) if direction == 0 else (b, a)
                        rows[u].add(v)
                    classes, mapping, parent = forest_model(rows)
                    file = case / "out" / f"relation-0-{direction}.rcf"
                    got = read_forest(file)
                    self.assertEqual((got["n"], got["m"]), (n, m))
                    self.assertEqual(got["membership"], [(u, c) for u, c in enumerate(mapping) if c is not None])
                    self.assertEqual(len(got["nodes"]), len(classes))
                    for c, node in enumerate(got["nodes"]):
                        rep, weight, p, depth, tin, tout = node
                        self.assertEqual(rep, mapping.index(c))
                        self.assertEqual(weight, mapping.count(c))
                        self.assertEqual(p, parent[c] if parent[c] is not None else 2**32-1)
                        ancestors = sum(ancestor(parent, a, c) for a in range(len(classes)))
                        self.assertEqual(depth, ancestors-1)
                        for d, other in enumerate(got["nodes"]):
                            self.assertEqual(tin <= other[4] < tout, ancestor(parent, c, d))
                    stat = report["views"][direction]
                    within = sum(mapping[u] is not None and mapping[u] == mapping[v]
                                 for u, v in itertools.combinations(range(n), 2))
                    cross = sum(mapping[u] != mapping[v] and comparable(parent, mapping[u], mapping[v])
                                for u, v in itertools.combinations(range(n), 2))
                    self.assertEqual(stat["within_class_pairs"], within)
                    self.assertEqual(stat["comparable_cross_class_pairs"], cross)
                    self.assertEqual(stat["forest_bytes"], file.stat().st_size)
                    actual_total += file.stat().st_size
                self.assertEqual(report["forest_bytes"], actual_total)
            last = root / str(len(relations)-1)
            execute([binary, last / "hin", last / "out"], root / "refuse.log",
                    reject_message="already exist")
            self.assertTrue((last / "out/structure.json").is_file())


if __name__ == "__main__":
    unittest.main()
