"""Fixed tiny mechanism probes, NOT a HINSCAN wall-time benchmark.

Print JSON only; never load a dataset or save an index. Explicit projection and
residual oracle are test instrumentation, excluded from planner counters.
"""
import json

from audit_semantics import fixtures, full_path_rows, incidence, reference
from majority_group_index import MajorityIndex
from layered_group_model import LayeredPlanner
from test_layered_group_model import merge_reference


def cases():
    for n in (12,24,48):
        yield 'four_center_majority',n,[n,4],[(0,1,[(u,b) for u in range(n)
              for b in range(4) if b != u%4])],'A-B-A','1',n
        pairs = [(u,b) for u in range(n) for b in range(3) if b != u%3]
        yield 'three_center_scalar',n,[n,3],[(0,1,pairs)],'A-B-A','1/2',2
        yield 'three_center_missed',n,[n,3],[(0,1,pairs)],'A-B-A','1',n
        for name,edges in [('matching',[(u,u+1) for u in range(0,n,2)]),
                            ('cycle',[(u,(u+1)%n) for u in range(n)])]:
            counts,relations = incidence(n,edges)
            yield name,n,counts,relations,'A-B-A','1/2',2
    fixture = next(f for f in fixtures() if f['name']=='hub_via_border_neighbors')
    counts,relations = incidence(fixture['n'],fixture['edges'])
    yield fixture['name'],fixture['n'],counts,relations,'A-B-A',fixture['epsilon'],fixture['mu']


def measure():
    records = []
    for name,n,counts,relations,path,epsilon,mu in cases():
        index = MajorityIndex(counts,relations)
        plan = LayeredPlanner(index,path,epsilon,mu).run()
        rows = full_path_rows(counts,relations,path)
        actual = merge_reference(index,path,epsilon,mu,plan)
        if actual != reference(rows,epsilon,mu):
            raise AssertionError(name+' failed complete-result check')
        record = dict(name=name,n=n,path=path,epsilon=epsilon,mu=mu,
                      projected_edges=sum(len(r)-1 for r in rows)//2,
                      completed_sources=len(plan.completed),
                      residual_sources=sum(map(len,plan.residual_components)),
                      planner_raw_reads=sum(plan.stats.raw_reads.values()),
                      planner_member_reads=sum(plan.stats.member_reads.values()),
                      stats={key:dict(value) if isinstance(value,dict) else value
                             for key,value in vars(plan.stats).items()},correct=True)
        record['offline'] = dict(
            raw_edges=sum(len(pairs) for a,b,pairs in index.relations),
            directed_group_member_slots=sum(len(g.members) for d in index.directions.values() for g in d.groups),
            directed_group_headers=sum(len(d.groups) for d in index.directions.values()),
            directed_degree_scalars=sum(len(d.closed_degrees) for d in index.directions.values()),
            ancestor_visits=sum(d.ancestor_visits for d in index.directions.values()),
            degree_posting_reads=sum(d.degree_posting_reads for d in index.directions.values()))
        records.append(record)
    return dict(scope='tiny mechanism audit, not production PSCAN or runtime benchmark',
                counters='Raw adjacency entries, group member entries and DSU calls; not all CPU operations. '
                         'Allocation, scalar/vertex visits, sorting and output are not time estimates. '
                         'Residual exact reference work is not included in planner counters.',
                index_size='Member/header/scalar counts only; not serialized bytes or Python heap size.',
                cases=records)


if __name__ == '__main__':
    print(json.dumps(measure(),indent=2))
