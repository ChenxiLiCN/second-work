"""Tiny independent oracle tests. Explicit projections exist ONLY in test code."""
import itertools
import json
import random
import unittest
from fractions import Fraction

from audit_semantics import fixtures, full_path_rows, incidence, reference
from majority_group_index import MajorityIndex
from layered_group_model import LayeredPlanner, Stats, connect_layers


def merge_reference(index, path, epsilon, mu, plan):
    """TEST ONLY: residual oracle, not the production PSCAN integration."""
    rows = full_path_rows(index.counts, index.relations, path)
    n = len(rows)
    result = dict(cores=[False]*n, memberships=[[] for _ in rows], roles=['outlier']*n)
    seen = set()
    for u, (role, cluster) in plan.completed.items():
        seen.add(u)
        result['cores'][u] = role == 'core'
        result['roles'][u] = role
        result['memberships'][u] = [] if cluster is None else [cluster]
    for vertices in plan.residual_components:
        local = {u: i for i, u in enumerate(vertices)}
        assert all(rows[u] <= set(vertices) for u in vertices), 'split projection component'
        part = reference([{local[v] for v in rows[u]} for u in vertices], epsilon, mu)
        for i, u in enumerate(vertices):
            assert u not in seen
            seen.add(u)
            result['cores'][u] = part['cores'][i]
            result['roles'][u] = part['roles'][i]
            result['memberships'][u] = [vertices[c] for c in part['memberships'][i]]
    assert seen == set(range(n))
    return result


def projection_components(rows):
    unseen, result = set(range(len(rows))), []
    while unseen:
        todo = [min(unseen)]
        unseen.remove(todo[0])
        for u in todo:
            for v in sorted(rows[u] & unseen):
                unseen.remove(v)
                todo.append(v)
        result.append(tuple(sorted(todo)))
    return tuple(result)


class OfflineGroupTests(unittest.TestCase):
    def test_all_4_by_3_relations_group_bound_and_clique(self):
        positions = list(itertools.product(range(4), range(3)))
        for bits in range(1 << len(positions)):
            pairs = [p for i,p in enumerate(positions) if bits & (1 << i)]
            index = MajorityIndex([4,3], [(0,1,pairs)])
            for direction in index.directions.values():
                self.assertLessEqual(sum(len(g.members) for g in direction.groups),
                                     sum(max(0,2*len(row)-1) for row in direction.rows))
                for g in direction.groups:
                    self.assertEqual(g.members, tuple(u for u,row in enumerate(direction.rows)
                        if 2*sum(g.lo <= v < g.hi for v in row) > g.hi-g.lo))
                    for u,v in itertools.combinations(g.members,2):
                        self.assertTrue(set(direction.rows[u]) & set(direction.rows[v]))
                reverse = index.directions[direction.target, direction.source].rows
                for u,row in enumerate(direction.rows):
                    expected = {u}.union(*(set(reverse[v]) for v in row))
                    self.assertEqual(direction.closed_degrees[u],len(expected))

    def test_sparse_ancestor_builder_and_dedup(self):
        index = MajorityIndex([1,8],[(0,1,[(0,0),(0,1),(0,2),(0,2)])])
        d = index.directions[0,1]
        self.assertEqual({(g.lo,g.hi) for g in d.groups},
                         {(0,1),(1,2),(2,3),(0,2),(0,4)})
        self.assertEqual(d.ancestor_visits,12)
        self.assertEqual(d.rows,((0,1,2),))

    def test_invalid_schema_and_edges(self):
        for counts,relations in [([2],[(0,0,[(0,1)])]),([2,2],[(0,1,[(2,0)])]),
                                  ([2,2],[(0,1,[]),(1,0,[])]),([-1,2],[])]:
            with self.assertRaises(ValueError):
                MajorityIndex(counts,relations)


class LayeredGroupTests(unittest.TestCase):
    def test_probe_report_is_json_and_preserves_counter_values(self):
        from measure_layered_group_model import measure
        report = json.loads(json.dumps(measure()))
        self.assertEqual(len(report['cases']),16)
        for case in report['cases']:
            self.assertTrue(case['correct'])
            self.assertEqual(case['planner_raw_reads'],sum(case['stats']['raw_reads'].values()))
            self.assertEqual(case['planner_member_reads'],sum(case['stats']['member_reads'].values()))

    def check_case(self,counts,relations,path,epsilon,mu):
        index = MajorityIndex(counts,relations)
        before = index.fingerprint()
        certs = []
        planner = LayeredPlanner(index,path,epsilon,mu,audit=certs.append)
        plan = planner.run()
        rows = full_path_rows(counts,relations,path)
        self.assertEqual(plan.components,projection_components(rows))
        self.assertEqual(merge_reference(index,path,epsilon,mu,plan),reference(rows,epsilon,mu))
        self.assertEqual(index.fingerprint(),before)
        # Inspect actual lifted sets in this independent test, never in planner.
        ids = [ord(c)-65 for c in path.split('-')]
        prefix = ids[:len(ids)//2]
        preimages = [set() for _ in range(counts[prefix[-1]])]
        for u in range(counts[ids[0]]):
            current = {u}
            for a,b in zip(prefix,prefix[1:]):
                current = {v for x in current for v in index.directions[a,b].rows[x]}
            for b in current:
                preimages[b].add(u)
        e = Fraction(epsilon)
        for cert in certs:
            lift = set().union(*(preimages[b] for b in cert.members))
            self.assertGreaterEqual(len(lift),cert.lower)
            self.assertGreaterEqual(cert.lower,mu)
            self.assertTrue(lift)
            self.assertGreaterEqual(cert.upper,max(len(rows[u]) for u in lift))
            for u,v in itertools.combinations(lift,2):
                self.assertIn(v,rows[u])
                self.assertGreaterEqual(len(rows[u]&rows[v])**2*e.denominator**2,
                                       e.numerator**2*len(rows[u])*len(rows[v]))
        return plan

    def test_distributed_witness_clique_completes_before_profiles(self):
        plan = self.check_case([4,4],[(0,1,[(u,b) for u in range(4)
                              for b in range(4) if b != u])],'A-B-A','1',4)
        self.assertFalse(plan.residual_components)
        self.assertEqual(plan.completed,{u:('core',0) for u in range(4)})
        self.assertEqual(plan.stats.raw_reads.get('profiles',0),0)

    def test_existing_semantic_fixtures(self):
        for fixture in fixtures():
            with self.subTest(fixture=fixture['name']):
                counts,relations = incidence(fixture['n'],fixture['edges'],fixture.get('duplicate',False))
                self.check_case(counts,relations,fixture.get('path','A-B-A'),fixture['epsilon'],fixture['mu'])

    def test_scalar_certificates_really_complete_short_and_long_paths(self):
        for prefix_length in [0,1,2]:
            counts = [12]*(prefix_length+1)+[3]
            relations = [(i,i+1,[(u,u) for u in range(12)]) for i in range(prefix_length)]
            relations.append((prefix_length,prefix_length+1,
                              [(u,b) for u in range(12) for b in range(3) if b != u%3]))
            types = list(range(prefix_length+2))+list(range(prefix_length,-1,-1))
            path = '-'.join(chr(65+t) for t in types)
            plan = self.check_case(counts,relations,path,'1/2',2)
            self.assertGreater(plan.stats.accepted_groups,0)
            self.assertEqual(plan.stats.early_core_vertices,0)
            self.assertEqual(plan.stats.scalar_core_vertices,12)
            self.assertFalse(plan.residual_components)

    def test_conservative_group_miss_must_retain_whole_clique(self):
        # Three-center domain is padded to four. Two-of-three is NOT a root majority.
        plan = self.check_case([12,3],[(0,1,[(u,b) for u in range(12)
                               for b in range(3) if b != u%3])],'A-B-A','1',12)
        self.assertEqual(plan.residual_components,(tuple(range(12)),))
        self.assertFalse(plan.completed)

    def test_unreachable_group_member_cannot_merge_targets(self):
        dsu,active,offsets = connect_layers((((0,),(1,)),),(2,3),Stats(),'ghost',
                                            groups=((0,2),(1,2)))
        self.assertEqual(active[0],[True,True])
        self.assertNotEqual(dsu.find(0),dsu.find(1))
        self.assertFalse(active[1][2])

    def test_dead_half_path_bridge_does_not_merge_projection(self):
        # b2 is reachable from both sources, but has no terminal successor.
        self.check_case([2,3,2],[(0,1,[(0,0),(0,2),(1,1),(1,2)]),
                                     (1,2,[(0,0),(1,1)])],'A-B-C-B-A','1',2)

    def test_large_mu_completes_no_core_without_pair_storage(self):
        plan = self.check_case([48,1],[(0,1,[(u,0) for u in range(47)])],'A-B-A','1',48)
        self.assertFalse(plan.residual_components)
        self.assertTrue(all(role == 'outlier' for role,c in plan.completed.values()))

    def test_empty_domains_and_reverse_target(self):
        self.check_case([0,2],[(0,1,[])],'A-B-A','1',2)
        self.check_case([3,0],[(0,1,[])],'A-B-A','1',2)
        self.check_case([1,5],[(0,1,[(0,v) for v in range(4)])],'B-A-B','1',4)

    def test_random_long_and_repeated_paths(self):
        rng = random.Random(913)
        for case in range(600):
            length = 1 + case%4
            counts = [rng.randrange(1,7) for _ in range(length+1)]
            relations = [(i,i+1,[(u,v) for u in range(counts[i]) for v in range(counts[i+1])
                          if rng.random() < (0.15,0.4,0.8)[case%3]]) for i in range(length)]
            types = list(range(length+1))+list(range(length-1,-1,-1))
            path = '-'.join(chr(65+i) for i in types)
            for e,mu in [('1/2',2),('9/10',3),('1',counts[0]+1)]:
                self.check_case(counts,relations,path,e,mu)
        counts,relations = incidence(6,[(0,1),(1,2),(2,3),(3,4),(4,0)])
        for path in ['A-B-A-B-A','A-B-A-B-A-B-A','B-A-B-A-B']:
            for e,mu in [('1/2',2),('1',4)]:
                self.check_case(counts,relations,path,e,mu)

    def test_one_offline_index_serves_different_queries(self):
        index = MajorityIndex([3,3],[(0,1,[(0,0),(0,1),(1,1),(2,2)])])
        digest = index.fingerprint()
        for path in ['A-B-A','B-A-B','A-B-A-B-A']:
            for e,mu in [('1/2',2),('1',3)]:
                LayeredPlanner(index,path,e,mu).run()
                self.assertEqual(index.fingerprint(),digest)

    def test_cyclic_schema_repeated_type_paths(self):
        rng = random.Random(914)
        paths = ['A-B-C-A-C-B-A','B-C-A-B-A-C-B','A-C-B-A-B-A-B-C-A']
        for case in range(150):
            counts = [rng.randrange(1,6) for _ in range(3)]
            relations = [(a,b,[(u,v) for u in range(counts[a]) for v in range(counts[b])
                              if rng.random() < (0.2,0.5,0.8)[case%3]])
                         for a,b in [(0,1),(1,2),(2,0)]]
            for path in paths:
                self.check_case(counts,relations,path,'3/4',3)

    def test_merge_completed_core_component_with_residual_roles(self):
        fixture = next(f for f in fixtures() if f['name']=='hub_via_border_neighbors')
        counts,relations = incidence(fixture['n'],fixture['edges'])
        witness = counts[1]
        pairs = relations[0][2]+[(u,witness) for u in range(14,20)]
        plan = self.check_case([20,witness+1],[(0,1,pairs)],'A-B-A',fixture['epsilon'],fixture['mu'])
        self.assertTrue(plan.residual_components)
        self.assertTrue(all(plan.completed[u]==('core',14) for u in range(14,20)))

    def test_relabeling_and_exact_epsilon_boundary(self):
        counts,rels = incidence(5,[(0,1),(0,2),(1,2),(2,3)])
        permutation = [3,1,4,0,2]
        relabeled = [(a,b,[(permutation[u],v) for u,v in pairs]) for a,b,pairs in rels]
        for e in ['1','999999999999999999/1000000000000000000','1/2']:
            self.check_case(counts,rels,'A-B-A',e,3)
            self.check_case(counts,relabeled,'A-B-A',e,3)

    def test_invalid_queries_are_not_fast_accepted(self):
        index = MajorityIndex([2,2],[(0,1,[(0,0),(1,0)])])
        for path,e,mu in [('A-B','1',2),('A-B-B','1',2),('A-C-A','1',2),
                           ('A-B-A','0',2),('A-B-A','2',2),('A-B-A','1',1),
                           ('A-B-A','1',2.5)]:
            with self.subTest(path=path,e=e,mu=mu), self.assertRaises(ValueError):
                LayeredPlanner(index,path,e,mu).run()


if __name__ == '__main__':
    unittest.main()
