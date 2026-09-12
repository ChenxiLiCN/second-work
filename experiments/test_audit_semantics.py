"""Hand-checked tests for the independent semantic audit, not a benchmark."""
import itertools
from pathlib import Path
import tempfile
import unittest
from run_server_ablation import reject_unmigrated_semantics
from verify_hinscan_semantics import verify

from audit_semantics import (adjacency, audit, fixtures, full_path_rows,
                             incidence, read_binary, read_result, read_roles,
                             reference, write_binary)


class SemanticReferenceTests(unittest.TestCase):
    def evaluate(self, name, **kwargs):
        case = next(c for c in fixtures() if c["name"] == name)
        counts, relations = incidence(case["n"], case["edges"], case.get("duplicate", False))
        rows = full_path_rows(counts, relations, case.get("path", "A-B-A"))
        return reference(rows, case["epsilon"], case["mu"], **kwargs)

    def test_mu_counts_self(self):
        self.assertEqual(self.evaluate("triangle_mu_boundary")["cores"], [True] * 3)
        self.assertEqual(self.evaluate("triangle_mu_boundary", include_self=False)["cores"],
                         [False] * 3)

    def test_unassigned_hub(self):
        r = self.evaluate("unassigned_hub")
        self.assertEqual(r["cores"], [True] * 8 + [False] * 2)
        self.assertEqual(r["memberships"][8], [])
        self.assertEqual(r["roles"][8:], ["hub", "outlier"])

    def test_overlapping_border_is_not_hub(self):
        r = self.evaluate("overlapping_border")
        self.assertEqual(r["cores"], [True] * 10 + [False])
        self.assertEqual(r["memberships"][10], [0, 5])
        self.assertEqual(r["roles"][10], "border")

    def test_hub_via_clustered_noncore_neighbors(self):
        r = self.evaluate("hub_via_border_neighbors")
        self.assertEqual(r["cores"], [True] * 10 + [False] * 4)
        self.assertEqual(r["memberships"][10:], [[0], [5], [], []])
        self.assertEqual(r["roles"][10:], ["border", "border", "hub", "outlier"])

    def test_single_border_and_isolates(self):
        self.assertEqual(self.evaluate("single_border")["roles"],
                         ["core"] * 5 + ["border", "outlier"])
        self.assertEqual(self.evaluate("isolates")["roles"], ["outlier"] * 3)
        self.assertEqual(self.evaluate("mu_above_degree")["cores"], [False] * 3)

    def test_long_path_and_duplicates(self):
        c = next(c for c in fixtures() if c["name"] == "long_path_dedup")
        counts, relations = incidence(c["n"], c["edges"], True)
        self.assertEqual(full_path_rows(counts, relations, c["path"]),
                         [{0, 1, 2}, {0, 1, 2, 3}, {0, 1, 2, 3}, {1, 2, 3}, {4}])
        self.assertEqual(self.evaluate("long_path_dedup")["cores"],
                         [False, True, True, False, False])

    def test_exact_similarity_boundary(self):
        # Adjacent 0,1 have intersection 2 and both closed degrees 3: sigma=2/3.
        rows = adjacency(4, [(0, 1), (1, 2), (2, 3), (3, 0)])
        self.assertEqual(reference(rows, "2/3", 2)["cores"], [True] * 4)
        self.assertEqual(reference(rows, "0.666666666666666667", 2)["cores"],
                         [False] * 4)

    def test_all_four_vertex_graphs_projection_and_mu_mapping(self):
        possible = list(itertools.combinations(range(4), 2))
        for mask in range(1 << len(possible)):
            edges = [edge for i, edge in enumerate(possible) if mask & (1 << i)]
            rows = adjacency(4, edges)
            counts, relations = incidence(4, edges, True)
            self.assertEqual(full_path_rows(counts, relations, "A-B-A"), rows)
            for epsilon in ("0.5", "0.8", "1"):
                for mu in range(2, 6):
                    self.assertEqual(reference(rows, epsilon, mu),
                                     reference(rows, epsilon, mu - 1, include_self=False))

    def test_invalid_scope_and_parameters(self):
        with self.assertRaises(ValueError):
            reference([{0, 1}, {1}], "0.5", 2)
        for epsilon, mu in (("0", 2), ("1.1", 2), ("0.5", 1)):
            with self.assertRaises(ValueError):
                reference([{0}], epsilon, mu)
        with self.assertRaises(ValueError):
            full_path_rows([1, 1], [(0, 1, [(0, 0)]), (0, 1, [(0, 0)])], "A-B-A")

    def test_binary_roundtrip_and_corruption(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / "binary"
            rows = adjacency(4, [(0, 1), (1, 2)])
            write_binary(target, rows)
            self.assertEqual(read_binary(target), rows)
            (target / "b_degree.bin").write_bytes(b"x")
            with self.assertRaises(ValueError):
                read_binary(target)

    def test_strict_result_parser(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "result.txt"
            path.write_text("c/n vertex_id cluster_id\nn 2 0\nn 2 1\n", encoding="utf-8")
            self.assertEqual(read_result(path, 3)["memberships"], [[], [], [0, 1]])
            for records in ("c 0 0\nc 0 0\n", "c 0 0\nn 0 1\n", "n 0 1\nc 0 0\n"):
                path.write_text("c/n vertex_id cluster_id\n" + records, encoding="utf-8")
                with self.assertRaises(ValueError):
                    read_result(path, 3)

    def test_strict_roles_parser(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "roles.txt"
            header = "vertex_id role cluster_count clusters\n"
            for records in ("0 outlier 0\n", "0 outlier 0\n0 outlier 0\n",
                            "0 border 2 1\n1 core 1 1\n"):
                path.write_text(header + records, encoding="utf-8")
                with self.assertRaises(ValueError):
                    read_roles(path, 2)

    def test_existing_output_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            sentinel = Path(tmp) / "keep.txt"
            sentinel.write_text("keep", encoding="utf-8")
            with self.assertRaises(FileExistsError):
                audit(Path(tmp), Path(tmp))
            with self.assertRaises(FileExistsError):
                verify(Path(tmp), Path(tmp))
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "keep")

    def test_historical_runner_rejects_mixed_semantics(self):
        reject_unmigrated_semantics({"block_mode": "9"})
        for info in ({"semantics_version": "hinscan_nonindependent_v1"},
                     {"mu_counts_self": "1"}):
            with self.assertRaisesRegex(RuntimeError, "not migrated"):
                reject_unmigrated_semantics(info)


if __name__ == "__main__":
    unittest.main()
