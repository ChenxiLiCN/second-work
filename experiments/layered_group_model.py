"""Whole-component completion before per-source factors, not a PSCAN replacement.

This planner never constructs projected rows, lifted per-group source lists or
similarity edge tables. Residual WHOLE components require an exact caller.
"""
from collections import Counter
from dataclasses import dataclass, field
from fractions import Fraction


@dataclass
class Stats:
    raw_reads: Counter = field(default_factory=Counter)
    member_reads: Counter = field(default_factory=Counter)
    dsu_calls: Counter = field(default_factory=Counter)
    accepted_groups: int = 0
    early_core_vertices: int = 0
    scalar_core_vertices: int = 0
    small_component_vertices: int = 0


class DSU:
    def __init__(self,n,stats,phase):
        self.parent = list(range(n))
        self.size = [1]*n
        self.stats,self.phase = stats,phase

    def find(self,u):
        self.stats.dsu_calls[self.phase] += 1
        while self.parent[u] != u:
            self.parent[u] = self.parent[self.parent[u]]
            u = self.parent[u]
        return u

    def union(self,u,v):
        u,v = self.find(u),self.find(v)
        if u != v:
            if self.size[u] < self.size[v]:
                u,v = v,u
            self.parent[v] = u
            self.size[u] += self.size[v]


def connect_layers(rows,sizes,stats,phase,*,groups=None,sources=None):
    """Trim directed layered paths, then union; group lifts share these paths.

    With groups=None terminal vertices are separate centers. Otherwise each
    group adds a node linked ONLY to its reachable terminal members. Distinct
    positions remain distinct even when a meta-path repeats a vertex type.
    """
    offsets = [0]
    for size in sizes:
        offsets.append(offsets[-1]+size)
    forward = [[False]*n for n in sizes]
    forward[0] = [True]*sizes[0] if sources is None else list(sources)
    for layer,matrix in enumerate(rows):
        for u,row in enumerate(matrix):
            if forward[layer][u]:
                for v in row:
                    stats.raw_reads[phase] += 1
                    forward[layer+1][v] = True
    backward = [[False]*n for n in sizes]
    if groups is None:
        backward[-1] = [True]*sizes[-1]
    else:
        for group in groups:
            for b in group:
                stats.member_reads[phase] += 1
                backward[-1][b] = True
    for layer in range(len(rows)-1,-1,-1):
        for u,row in enumerate(rows[layer]):
            if forward[layer][u]:
                for v in row:
                    stats.raw_reads[phase] += 1
                    if backward[layer+1][v]:
                        backward[layer][u] = True
    active = [[f and b for f,b in zip(fs,bs)] for fs,bs in zip(forward,backward)]
    dsu = DSU(offsets[-1]+(0 if groups is None else len(groups)),stats,phase)
    for layer,matrix in enumerate(rows):
        for u,row in enumerate(matrix):
            if active[layer][u]:
                for v in row:
                    stats.raw_reads[phase] += 1
                    if active[layer+1][v]:
                        dsu.union(offsets[layer]+u,offsets[layer+1]+v)
    if groups is not None:
        for i,group in enumerate(groups):
            for b in group:
                stats.member_reads[phase] += 1
                if active[-1][b]:
                    dsu.union(offsets[-2]+b,offsets[-1]+i)
    return dsu,active,offsets


def _walk_upper(rows,sizes,cap,stats):
    values = [1]*sizes[-1]
    for matrix in reversed(rows):
        previous = []
        for row in matrix:
            total = 0
            for v in row:
                stats.raw_reads['profiles'] += 1
                total = min(cap,total+values[v])
            previous.append(total)
        values = previous
    return values


@dataclass(frozen=True)
class Certificate:
    members: tuple
    lower: int
    upper: int


@dataclass
class Plan:
    components: tuple
    completed: dict
    residual_components: tuple
    stats: Stats


class LayeredPlanner:
    def __init__(self,index,path,epsilon,mu,*,audit=None):
        tokens = path.split('-')
        if any(len(t) != 1 or not 'A' <= t <= 'Z' for t in tokens):
            raise ValueError('model requires A-Z type names')
        ids = tuple(ord(t)-65 for t in tokens)
        if len(ids) < 3 or len(ids)%2 != 1 or ids != ids[::-1]:
            raise ValueError('model requires symmetric even-edge paths')
        if any((a,b) not in index.directions for a,b in zip(ids,ids[1:])):
            raise ValueError('unsupported or missing transition')
        epsilon = Fraction(epsilon)
        if not 0 < epsilon <= 1 or type(mu) is not int or mu < 2:
            raise ValueError('require 0<epsilon<=1 and integer mu>=2 including self')
        self.index,self.ids,self.epsilon,self.mu = index,ids,epsilon,mu
        self.audit = audit

    def run(self):
        index,ids,e,mu = self.index,self.ids,self.epsilon,self.mu
        stats = Stats()
        half = ids[:len(ids)//2+1]
        sizes = tuple(index.counts[t] for t in half)
        matrices = tuple(index.directions[a,b].rows for a,b in zip(half,half[1:]))
        n = sizes[0]
        dsu,active,offsets = connect_layers(matrices,sizes,stats,'components')
        by_root = {}
        for u in range(n):
            by_root.setdefault(dsu.find(u),[]).append(u)
        components = tuple(sorted(tuple(us) for us in by_root.values()))
        root_component = {dsu.find(us[0]):i for i,us in enumerate(components)}
        source_component = [0]*n
        for i,us in enumerate(components):
            for u in us:
                source_component[u] = i
        completed = {}
        remaining = set(range(len(components)))

        def finish(i,core,phase):
            us = components[i]
            completed.update((u,('core',us[0]) if core else ('outlier',None)) for u in us)
            remaining.remove(i)
            setattr(stats,phase,getattr(stats,phase)+len(us))

        for i,us in enumerate(components):
            if len(us) < mu:
                finish(i,False,'small_component_vertices')
        if not remaining:
            return Plan(components,completed,(),stats)

        # Every reachable member of an inner majority group is in one component.
        terminal_comp = [-1]*sizes[-2]
        terminal_counts = Counter()
        for b in range(sizes[-2]):
            if active[-2][b]:
                c = root_component[dsu.find(offsets[-3]+b)]
                terminal_comp[b] = c
                terminal_counts[c] += 1
        groups = index.directions[half[-2],half[-1]].groups
        group_components = []
        for group in groups:
            c,count = -1,0
            for b in group.members:
                stats.member_reads['coverage'] += 1
                if terminal_comp[b] >= 0:
                    if c >= 0 and c != terminal_comp[b]:
                        raise AssertionError('inner clique straddles projection components')
                    c = terminal_comp[b]
                    count += 1
            group_components.append(c)
            if c in remaining and count == terminal_counts[c]:
                finish(c,True,'early_core_vertices')
        if not remaining:
            return Plan(components,completed,(),stats)

        prefix,prefix_sizes = matrices[:-1],sizes[:-1]
        if not prefix:
            upper = index.directions[half[-2],half[-1]].closed_degrees
        else:
            full_rows = tuple(index.directions[a,b].rows for a,b in zip(ids,ids[1:]))
            full_sizes = tuple(index.counts[t] for t in ids)
            walk = _walk_upper(full_rows,full_sizes,n,stats)
            upper = tuple(min(len(components[source_component[u]]),max(1,walk[u])) for u in range(n))
        q_upper = _walk_upper(prefix,prefix_sizes,prefix_sizes[-1],stats)
        delta = [0]*len(components)
        for u in range(n):
            c = source_component[u]
            delta[c] = max(delta[c],q_upper[u])
        # Preimage lower bounds: exact first-hop indegree, then MAX (not SUM).
        lower = [1]*n
        maximum = list(upper)
        for layer,matrix in enumerate(prefix):
            next_lower,next_max = [0]*prefix_sizes[layer+1],[0]*prefix_sizes[layer+1]
            for u,row in enumerate(matrix):
                for b in row:
                    stats.raw_reads['profiles'] += 1
                    next_lower[b] = next_lower[b]+1 if layer == 0 else max(next_lower[b],lower[u])
                    next_max[b] = max(next_max[b],maximum[u])
            lower,maximum = next_lower,next_max
        accepted = []
        for group,c in zip(groups,group_components):
            if c not in remaining:
                continue
            total,best,m = 0,0,0
            for b in group.members:
                stats.member_reads['certificates'] += 1
                total += lower[b]
                best = max(best,lower[b])
                m = max(m,maximum[b])
            divisor = min(len(group.members),delta[c])
            if divisor == 0:
                continue
            bound = max(best,(total+divisor-1)//divisor)
            if bound >= mu and bound*e.denominator >= m*e.numerator:
                accepted.append(group.members)
                if self.audit is not None:
                    self.audit(Certificate(group.members,bound,m))
        stats.accepted_groups = len(accepted)
        if accepted:
            sources = [source_component[u] in remaining for u in range(n)]
            certificate_dsu,certified,_ = connect_layers(prefix,prefix_sizes,stats,'lift',
                                                         groups=accepted,sources=sources)
            for c in sorted(remaining):
                us = components[c]
                if (all(certified[0][u] for u in us) and
                        len({certificate_dsu.find(u) for u in us}) == 1):
                    finish(c,True,'scalar_core_vertices')
        return Plan(components,completed,tuple(components[c] for c in sorted(remaining)),stats)
