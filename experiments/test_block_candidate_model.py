"""Tiny independent checks for the proposed block generator; never real data."""
import importlib.util
import itertools
from collections import Counter
from fractions import Fraction
import unittest

from audit_semantics import fixtures, full_path_rows, incidence, reference

_spec = importlib.util.find_spec("block_candidate_model")
if _spec is not None:
    import block_candidate_model as model
else:
    model = None


class BlockCandidateTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(model, "The independent block candidate model is not implemented")

    def check_case(self, counts, relations, path, epsilon, mu):
        rows = full_path_rows(counts, relations, path)
        expected = reference(rows, epsilon, mu)
        owner, emitted, actual_builds = self, set(), Counter()
        e = Fraction(epsilon)

        class AuditedQuery(model.BlockQuery):
            # Instrumentation stays in this test, never in the algorithm's state.
            def _consume(self, x, y):
                for u, v in itertools.product(range(*x), range(*y)):
                    owner.assertLess(u, v)
                    owner.assertNotIn((u, v), emitted, "duplicate accepted pair")
                    owner.assertIn(v, rows[u], "block contains a non-neighbor")
                    owner.assertGreaterEqual(len(rows[u] & rows[v])**2 * e.denominator**2,
                                             e.numerator**2 * len(rows[u]) * len(rows[v]))
                    emitted.add((u, v))
                super()._consume(x, y)

            def _reach(self, sources, phase):
                sources = tuple(sources)
                if phase == "candidate":
                    actual_builds[sources] += 1
                return super()._reach(sources, phase)

        index = model.RawBlockIndex(counts, relations)
        before = index.fingerprint()
        engine = AuditedQuery(index, path, epsilon, mu)
        result = engine.run()
        self.assertEqual(result, expected)
        self.assertEqual(index.fingerprint(), before, "query changed offline index")
        self.assertTrue(all(c == 1 for c in engine.stats.source_builds.values()))
        self.assertTrue(all(c == 1 for c in actual_builds.values()))
        self.assertLessEqual(engine.stats.pending_created, engine.n * (mu - 2))
        self.assertLessEqual(engine.stats.pending_peak, engine.stats.pending_created)
        return engine

    def test_isolated_vertex_self_and_mu(self):
        self.check_case([3, 0], [(0, 1, [])], "A-B-A", "1", 2)

    def test_query_target_need_not_be_first_type(self):
        engine = self.check_case([1, 5], [(0, 1, [(0, v) for v in range(4)])],
                                 "B-A-B", "1", 5)
        self.assertEqual(len(engine.pending), 6)

    def test_large_mu_can_retain_all_positive_pairs(self):
        # Document a limitation, not a claim of bounded/small query memory.
        engine = self.check_case([48, 1], [(0, 1, [(u, 0) for u in range(47)])],
                                 "A-B-A", "1", 48)
        self.assertFalse(any(engine.run_result["cores"]))
        self.assertEqual(len(engine.pending), 47 * 46 // 2)
        self.assertEqual(engine.stats.pending_peak, 1081)

    def test_existing_role_and_path_fixtures(self):
        for case in fixtures():
            with self.subTest(case=case["name"]):
                counts, relations = incidence(case["n"], case["edges"], case.get("duplicate", False))
                self.check_case(counts, relations, case.get("path", "A-B-A"),
                                case["epsilon"], case["mu"])

    def test_all_four_vertex_graphs(self):
        edges = list(itertools.combinations(range(4), 2))
        for mask in range(64):
            counts, relations = incidence(4, [edge for i, edge in enumerate(edges) if mask & (1 << i)])
            for epsilon, mu in itertools.product(("0.5", "0.8", "1"), (2, 3, 4, 5)):
                with self.subTest(mask=mask, epsilon=epsilon, mu=mu):
                    self.check_case(counts, relations, "A-B-A", epsilon, mu)

    def test_different_witnesses_can_certify_whole_clique(self):
        # Every pair of two-element subsets of a three-element domain intersects.
        n = 12
        edges = [(u, w) for u in range(n) for w in range(3) if w != u % 3]
        engine = self.check_case([n, 3], [(0, 1, edges)], "A-B-A", "0.9", 5)
        self.assertEqual(engine.stats.pair_checks, 0)
        self.assertEqual(engine.stats.candidate_reads, 0)
        self.assertEqual(engine.stats.pending_created, 0)
        self.assertEqual(engine.stats.clique_fastpaths, 1)

    def test_union_nonempty_does_not_mean_full_rectangle(self):
        # The union for sources 0,1 contains 2,3, but 0->3 and 1->2 do not exist.
        counts, relations = incidence(4, [(0, 2), (1, 3)])
        engine = self.check_case(counts, relations, "A-B-A", "1", 2)
        self.assertEqual(engine.run_result["memberships"], [[0], [1], [0], [1]])

    def test_pending_triangle_retains_future_core_edge(self):
        counts, relations = incidence(5, [(0, 1), (1, 2), (0, 2), (0, 3), (2, 4)])
        engine = self.check_case(counts, relations, "A-B-A", "0.5", 4)
        self.assertEqual(engine.run_result["cores"], [True, False, True, False, False])
        self.assertEqual(engine.run_result["memberships"], [[0]] * 5)

    def test_invalid_mu_and_threshold_rejected(self):
        index = model.RawBlockIndex([2, 1], [(0, 1, [(0, 0), (1, 0)])])
        for epsilon, mu in (("0", 2), ("1.1", 2), ("0.5", 1), ("0.5", -1)):
            with self.subTest(epsilon=epsilon, mu=mu), self.assertRaises(ValueError):
                model.BlockQuery(index, "A-B-A", epsilon, mu)

    def test_long_path_with_missing_and_distinct_middle_endpoints(self):
        for assignment in itertools.product((-1, 0, 1), repeat=4):
            relations = [(0, 1, [(u, u) for u in range(4)]),
                         (1, 2, [(r, b) for r, b in enumerate(assignment) if b >= 0])]
            for epsilon, mu in itertools.product(("0.5", "1"), (2, 3, 5)):
                with self.subTest(assignment=assignment, epsilon=epsilon, mu=mu):
                    self.check_case([4, 4, 2], relations, "A-B-C-B-A", epsilon, mu)

    def test_composed_bounds_never_exclude_true_counts(self):
        cells = list(itertools.product(range(2), repeat=2))
        intervals = ((0, 1), (1, 2), (0, 2))
        for ma, mb in itertools.product(range(16), repeat=2):
            a = {edge for i, edge in enumerate(cells) if ma & (1 << i)}
            b = {edge for i, edge in enumerate(cells) if mb & (1 << i)}
            index = model.RawBlockIndex([2, 2, 2], [(0, 1, list(a)), (1, 2, list(b))])
            engine = model.BlockQuery(index, "A-B-C-B-A", "1", 2)
            for x, y in itertools.product(intervals, repeat=2):
                actual = {(u, v) for u in range(*x) for v in range(*y)
                          if any((u, w) in a and (w, v) in b for w in range(2))}
                rd = [sum(left == u for left, right in actual) for u in range(*x)]
                cd = [sum(right == v for left, right in actual) for v in range(*y)]
                bound = engine._bounds((0, 1, 2), x, y)
                self.assertLessEqual(bound.row_min, min(rd))
                self.assertGreaterEqual(bound.row_max, max(rd))
                self.assertLessEqual(bound.col_min, min(cd))
                self.assertGreaterEqual(bound.col_max, max(cd))

    def test_pending_triangle_in_prescribed_arrival_order(self):
        edges = [(0, 1), (1, 2), (0, 2), (0, 3), (2, 4)]
        counts, relations = incidence(5, edges)
        engine = model.BlockQuery(model.RawBlockIndex(counts, relations), "A-B-A", "0.5", 4)
        for u, v in edges[:3]:
            engine._consume((u, u+1), (v, v+1))
        self.assertFalse(any(engine.core))
        self.assertIn((0, 2), engine.pending)
        engine._consume((0, 1), (3, 4))
        self.assertTrue(engine.core[0])
        self.assertFalse(engine.core[2])
        self.assertIn((0, 2), engine.pending)
        engine._consume((2, 3), (4, 5))
        self.assertEqual(engine.core, [True, False, True, False, False])
        self.assertEqual(engine._find(0), engine._find(2))
        self.assertNotIn((0, 2), engine.pending)

    def test_same_offline_index_serves_multiple_queries(self):
        counts, relations = incidence(5, [(0, 1), (1, 2), (2, 3)])
        index = model.RawBlockIndex(counts, relations)
        before = index.fingerprint()
        for path in ("A-B-A", "A-B-A-B-A"):
            for epsilon, mu in (("0.5", 2), ("1", 3), ("0.8", 9)):
                result = model.BlockQuery(index, path, epsilon, mu).run()
                self.assertEqual(result, reference(full_path_rows(counts, relations, path), epsilon, mu))
                self.assertEqual(index.fingerprint(), before)

    def test_operation_report_is_count_only_and_checks_results(self):
        self.assertIsNotNone(importlib.util.find_spec("measure_block_candidate_model"),
                             "The fixed tiny operation report is not implemented")
        import measure_block_candidate_model as probe
        report = probe.run_probe()
        self.assertFalse(report["timing_benchmark"])
        self.assertTrue(report["all_matched"])
        self.assertEqual(len(report["cases"]), 9)
        for case in report["cases"]:
            self.assertLessEqual(case["n"], 48)
            self.assertTrue(case["matched"])
            self.assertGreater(case["reference_projection_reads"], 0)
            self.assertEqual(case["model_original_reads"], sum(case["reads_by_phase"].values()))


if __name__ == "__main__":
    unittest.main()
