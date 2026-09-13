"""Tiny executable model, NOT the production index or a timing benchmark.

Offline: original directed rows and relation-root degree summaries only.
Non-root summaries are scanned online and charged to proof_reads. A full
compressed rectangle index is deliberately not claimed by this model.
No projected graph is built up front or stored offline. Online pending positive
pairs are explicit: their O(n * mu) bound can be quadratic and can retain the
entire positive similarity graph. This is NOT an unconditional nonmaterializing
implementation. No complete per-vertex query-neighborhood table is cached.

Scope: symmetric paths of single-letter A-Z types, distinct adjacent types,
one unlabelled relation per unordered type pair, synthesized inverse relations.
These are isolated verifier limitations, not changes to the research problem.
"""
from bisect import bisect_left
from dataclasses import dataclass, field
from fractions import Fraction
import hashlib


@dataclass(frozen=True)
class Bounds:
    row_min: int
    row_max: int
    col_min: int
    col_max: int


@dataclass
class Stats:
    source_builds: dict = field(default_factory=dict)
    degree_reads: int = 0
    degree_vertices: int = 0
    candidate_reads: int = 0
    candidate_vertices: int = 0
    exact_reads: int = 0
    exact_vertices: int = 0
    role_reads: int = 0
    role_vertices: int = 0
    proof_reads: int = 0
    proof_vertices: int = 0
    proof_calls: int = 0
    summary_hits: int = 0
    candidate_endpoints: int = 0
    mask_items: int = 0
    sort_items: int = 0
    peak_frontier: int = 0
    pair_checks: int = 0
    similar_blocks: int = 0
    similar_pairs_represented: int = 0
    clique_fastpaths: int = 0
    vertex_updates: int = 0
    union_attempts: int = 0
    pending_created: int = 0
    pending_peak: int = 0
    core_skip_checks: int = 0


class RawBlockIndex:
    """Query-independent baseline representation; raw roots, not all rectangles."""
    def __init__(self, counts, relations):
        self.counts = tuple(counts)
        if any(not isinstance(n, int) or n < 0 for n in counts):
            raise ValueError("invalid type sizes")
        rows = {}
        for a, b, pairs in relations:
            if a == b or not (0 <= a < len(counts) and 0 <= b < len(counts)):
                raise ValueError("tiny model requires distinct valid relation types")
            if (a, b) in rows or (b, a) in rows:
                raise ValueError("ambiguous type-only relation in tiny model")
            forward, reverse = [set() for _ in range(counts[a])], [set() for _ in range(counts[b])]
            for u, v in pairs:
                if not (0 <= u < counts[a] and 0 <= v < counts[b]):
                    raise ValueError("invalid original edge")
                forward[u].add(v)
                reverse[v].add(u)
            rows[a, b], rows[b, a] = forward, reverse
        self.rows = {key: tuple(tuple(sorted(row)) for row in value) for key, value in rows.items()}
        self.roots = {}
        for (a, b), forward in self.rows.items():
            rd = [len(row) for row in forward]
            cd = [len(row) for row in self.rows[b, a]]
            self.roots[a, b] = Bounds(min(rd, default=0), max(rd, default=0),
                                      min(cd, default=0), max(cd, default=0))

    def fingerprint(self):
        payload = repr((self.counts, sorted(self.rows.items()), sorted(self.roots.items())))
        return hashlib.sha256(payload.encode()).hexdigest()

    def rectangle(self, a, b, x, y, stats):
        if x == (0, self.counts[a]) and y == (0, self.counts[b]):
            stats.summary_hits += 1
            return self.roots[a, b]
        # This scan is ONLINE work, not a pretend constant-time index lookup.
        rd = []
        cd = [0] * (y[1] - y[0])
        stats.proof_vertices += (x[1] - x[0]) + len(cd)
        for u in range(*x):
            degree = 0
            for v in self.rows[a, b][u]:
                stats.proof_reads += 1
                if y[0] <= v < y[1]:
                    degree += 1
                    cd[v-y[0]] += 1
            rd.append(degree)
        return Bounds(min(rd, default=0), max(rd, default=0),
                      min(cd, default=0), max(cd, default=0))


class BlockQuery:
    """Source-once generator plus immediate-core reference scheduler.

Python per-vertex updates implement semantics, not the proposed interval-tree
performance. Only symmetric type paths are supported by this isolated model.
"""
    def __init__(self, index, path, epsilon, mu):
        self.index, self.epsilon, self.mu = index, Fraction(epsilon), mu
        if not 0 < self.epsilon <= 1 or not isinstance(mu, int) or isinstance(mu, bool) or mu < 2:
            raise ValueError("invalid epsilon or mu")
        parts = path.split("-")
        if any(len(p) != 1 or not "A" <= p <= "Z" for p in parts):
            raise ValueError("tiny model uses single-letter type names")
        self.path = tuple(ord(p)-ord("A") for p in parts)
        if len(self.path) < 3 or self.path != self.path[::-1]:
            raise ValueError("tiny verifier currently supports symmetric type paths only")
        if any((a, b) not in index.rows for a, b in zip(self.path, self.path[1:])):
            raise ValueError("path relation missing")
        self.n = index.counts[self.path[0]]
        self.stats = Stats()
        self.lower = [1] * self.n
        self.core = [False] * self.n
        self.parent = list(range(self.n))
        self.size = [1] * self.n
        self.minimum = list(range(self.n))
        self.pending = set()
        self.incident = [set() for _ in range(self.n)]
        self.degrees = []
        self.run_result = None

    def _reach(self, sources, phase):
        frontier = set(sources)
        self.stats.peak_frontier = max(self.stats.peak_frontier, len(frontier))
        for a, b in zip(self.path, self.path[1:]):
            following = set()
            setattr(self.stats, phase+"_vertices", getattr(self.stats, phase+"_vertices") + len(frontier))
            for u in frontier:
                row = self.index.rows[a, b][u]
                setattr(self.stats, phase+"_reads", getattr(self.stats, phase+"_reads") + len(row))
                following.update(row)
            self.stats.peak_frontier = max(self.stats.peak_frontier, len(frontier)+len(following))
            frontier = following
        return frontier

    def _bounds(self, path, x, y):
        self.stats.proof_calls += 1
        if len(path) == 2:
            return self.index.rectangle(path[0], path[-1], x, y, self.stats)
        mid = (len(path)-1)//2
        z = self.index.counts[path[mid]]
        if z == 0:
            return Bounds(0, 0, 0, 0)
        a = self._bounds(path[:mid+1], x, (0, z))
        b = self._bounds(path[mid:], (0, z), y)
        nx, ny = x[1]-x[0], y[1]-y[0]
        if a.row_min + b.col_min > z:
            return Bounds(ny, ny, nx, nx)
        return Bounds(b.row_min if a.row_min else 0, min(ny, a.row_max*b.row_max),
                      a.col_min if b.col_min else 0, min(nx, a.col_max*b.col_max))

    def _closed_status(self, x, w):
        bound = self._bounds(self.path, x, w)
        overlap = max(x[0], w[0]) < min(x[1], w[1])
        full = bound.row_min == w[1]-w[0] or (x == w and x[1]-x[0] == 1)
        empty = bound.row_max == 0 and not overlap
        return full, empty

    def _common_bounds(self, x, y, w):
        fx, ex = self._closed_status(x, w)
        fy, ey = self._closed_status(y, w)
        if ex or ey:
            return 0, 0
        if fx and fy:
            return w[1]-w[0], w[1]-w[0]
        if w[1]-w[0] == 1:
            return 0, 1
        mid = (w[0]+w[1])//2
        l0, u0 = self._common_bounds(x, y, (w[0], mid))
        l1, u1 = self._common_bounds(x, y, (mid, w[1]))
        return l0+l1, u0+u1

    def _block_decision(self, x, y):
        if self._bounds(self.path, x, y).row_min != y[1]-y[0]:
            return None
        low, high = self._common_bounds(x, y, (0, self.n))
        dx, dy = self.degrees[x[0]:x[1]], self.degrees[y[0]:y[1]]
        p, q = self.epsilon.numerator, self.epsilon.denominator
        if low*low*q*q >= p*p*max(dx)*max(dy):
            return True
        if high*high*q*q < p*p*min(dx)*min(dy):
            return False
        return None

    def _find(self, u):
        while self.parent[u] != u:
            self.parent[u] = self.parent[self.parent[u]]
            u = self.parent[u]
        return u

    def _join(self, u, v):
        self.stats.union_attempts += 1
        a, b = self._find(u), self._find(v)
        if a == b:
            return
        if self.size[a] < self.size[b]:
            a, b = b, a
        self.parent[b] = a
        self.size[a] += self.size[b]
        self.minimum[a] = min(self.minimum[a], self.minimum[b])

    def _same_core(self, x, y):
        root = None
        for u in itertools_chain_ranges(x, y):
            self.stats.core_skip_checks += 1
            if not self.core[u]:
                return False
            other = self._find(u)
            if root is None:
                root = other
            elif root != other:
                return False
        return root is not None

    def _defer(self, u, v):
        edge = (min(u, v), max(u, v))
        if edge in self.pending:
            raise AssertionError("candidate counted twice")
        self.pending.add(edge)
        self.incident[u].add(edge)
        self.incident[v].add(edge)
        self.stats.pending_created += 1
        self.stats.pending_peak = max(self.stats.pending_peak, len(self.pending))

    def _consume(self, x, y):
        """Disjoint, ordered, certified complete similar rectangle."""
        self.stats.similar_blocks += 1
        self.stats.similar_pairs_represented += (x[1]-x[0])*(y[1]-y[0])
        newly = []
        for side, increment in ((x, y[1]-y[0]), (y, x[1]-x[0])):
            for u in range(*side):
                if not self.core[u]:
                    self.stats.vertex_updates += 1
                    self.lower[u] += increment
                    if self.lower[u] >= self.mu:
                        self.core[u] = True
                        newly.append(u)
        for u in newly:
            for edge in tuple(self.incident[u]):
                a, b = edge
                if self.core[a] and self.core[b]:
                    self._join(a, b)
                    self.pending.remove(edge)
                    self.incident[a].remove(edge)
                    self.incident[b].remove(edge)
        cx = [u for u in range(*x) if self.core[u]]
        cy = [v for v in range(*y) if self.core[v]]
        if cx and cy:
            self._join(cx[0], cy[0])
            for u in cx[1:]:
                self._join(u, cy[0])
            for v in cy[1:]:
                self._join(cx[0], v)
        # Expand only still-pending relationships, never the core-core product.
        for u in range(*x):
            if not self.core[u]:
                for v in range(*y):
                    self._defer(u, v)
        for u in cx:
            for v in range(*y):
                if not self.core[v]:
                    self._defer(u, v)

    def _exact_pair(self, u, v, left):
        self.stats.pair_checks += 1
        right = self._reach((v,), "exact") | {v}
        common = len(left & right)
        p, q = self.epsilon.numerator, self.epsilon.denominator
        if common*common*q*q >= p*p*self.degrees[u]*self.degrees[v]:
            self._consume((u, u+1), (v, v+1))

    def _frame(self, x, remaining):
        if not remaining:
            return
        self.stats.source_builds[x] = self.stats.source_builds.get(x, 0)+1
        frontier = self._reach(range(*x), "candidate")
        self.stats.candidate_endpoints += len(frontier)
        self.stats.mask_items += len(frontier)+len(remaining)
        active = {v for v in frontier & remaining if v > x[0]}
        if not active:
            return
        self.stats.sort_items += len(active)
        ordered = sorted(active)
        unresolved = set()
        for y in canonical_cover(ordered, 0, self.n):
            if max(x[0], y[0]) < min(x[1], y[1]):
                unresolved.update(range(*y))
                continue
            if self._same_core(x, y):
                continue
            if x[1]-x[0] == 1 and y[1]-y[0] == 1:
                self._exact_pair(x[0], y[0], frontier | {x[0]})
                continue
            decision = self._block_decision(x, y)
            if decision is True:
                self._consume(x, y)
            elif decision is None:
                if x[1]-x[0] > 1:
                    unresolved.update(range(*y))
                else:
                    left = frontier | {x[0]}
                    for v in range(*y):
                        if not self._same_core(x, (v, v+1)):
                            self._exact_pair(x[0], v, left)
        # Only unresolved target masks survive into the source descendants.
        del frontier, active, ordered
        if unresolved and x[1]-x[0] > 1:
            mid = (x[0]+x[1])//2
            self._frame((x[0], mid), unresolved)
            self._frame((mid, x[1]), unresolved)

    def run(self):
        if self.run_result is not None:
            return self.run_result
        if not self.n or self.mu > self.n:
            self.run_result = dict(cores=[False]*self.n, memberships=[[] for _ in range(self.n)],
                                   roles=["outlier"]*self.n)
            return self.run_result
        root = (0, self.n)
        if self._bounds(self.path, root, root).row_min == self.n:
            self.stats.clique_fastpaths += 1
            self.run_result = dict(cores=[True]*self.n, memberships=[[0] for _ in range(self.n)],
                                   roles=["core"]*self.n)
            return self.run_result
        # Exact scalar degrees only; no table of query neighborhoods is retained.
        self.degrees = [len(self._reach((u,), "degree") | {u}) for u in range(self.n)]
        self._frame(root, set(range(self.n)))
        memberships = [set() for _ in range(self.n)]
        for u in range(self.n):
            if self.core[u]:
                memberships[u].add(self.minimum[self._find(u)])
        for a, b in self.pending:
            if self.core[a]:
                memberships[b].add(self.minimum[self._find(a)])
            elif self.core[b]:
                memberships[a].add(self.minimum[self._find(b)])
        roles = []
        for u in range(self.n):
            if self.core[u]:
                roles.append("core")
            elif memberships[u]:
                roles.append("border")
            else:
                touched = set()
                for v in self._reach((u,), "role"):
                    touched.update(memberships[v])
                    if len(touched) >= 2:
                        break
                roles.append("hub" if len(touched) >= 2 else "outlier")
        self.run_result = dict(cores=list(self.core), memberships=[sorted(s) for s in memberships], roles=roles)
        return self.run_result


def itertools_chain_ranges(x, y):
    yield from range(*x)
    yield from range(*y)


def canonical_cover(values, lo, hi):
    """Disjoint dyadic cover of sorted distinct values; no scan of empty IDs."""
    def visit(a, b, start, stop):
        if start == stop:
            return
        if stop-start == b-a:
            yield a, b
            return
        mid = (a+b)//2
        cut = bisect_left(values, mid, start, stop)
        yield from visit(a, mid, start, cut)
        yield from visit(mid, b, cut, stop)
    yield from visit(lo, hi, 0, len(values))
