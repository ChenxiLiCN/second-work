"""Query-independent original-relation groups. Tiny research prototype only.

No meta-path projected rows or pair similarities are retained. Exact roundtrip
degrees reproduce existing scalar metadata; building them may still be costly.
"""
from dataclasses import dataclass
import hashlib
from types import MappingProxyType


@dataclass(frozen=True)
class Group:
    lo: int
    hi: int
    members: tuple


@dataclass(frozen=True)
class Direction:
    source: int
    target: int
    rows: tuple
    groups: tuple
    closed_degrees: tuple
    ancestor_visits: int
    degree_posting_reads: int


def _direction(source, target, rows, reverse):
    width = 1 << max(0, (len(reverse)-1).bit_length())
    members = {}
    visits = 0
    for u,row in enumerate(rows):
        counts = {}
        for v in row:
            size = 1
            while size <= width:
                lo = v // size * size
                key = (lo,lo+size)
                counts[key] = counts.get(key,0)+1
                visits += 1
                size *= 2
        for (lo,hi),count in counts.items():
            if 2*count > hi-lo:
                members.setdefault((lo,hi),[]).append(u)
    groups = tuple(Group(lo,hi,tuple(us)) for (lo,hi),us in sorted(members.items()))
    degrees, degree_reads = [], 0
    for u,row in enumerate(rows):
        # One ephemeral neighborhood at a time; ONLY its degree is retained.
        reached = {u}
        for v in row:
            degree_reads += len(reverse[v])
            reached.update(reverse[v])
        degrees.append(len(reached))
    return Direction(source,target,rows,groups,tuple(degrees),visits,degree_reads)


class MajorityIndex:
    """Offline constructor deliberately accepts graph/schema only.

    Models distinct A-Z types with one relation per unordered type pair.
    Tuples/frozen values prevent online mutation of stored relations/groups.
    """
    def __init__(self, counts, relations):
        counts = tuple(counts)
        if not 1 <= len(counts) <= 26 or any(type(n) is not int or n < 0 for n in counts):
            raise ValueError('model requires 1..26 nonnegative type sizes')
        directions, canonical = {}, []
        for a,b,pairs in relations:
            if (type(a) is not int or type(b) is not int or
                    not 0 <= a < len(counts) or not 0 <= b < len(counts) or a == b):
                raise ValueError('model requires distinct valid endpoint types')
            if (a,b) in directions:
                raise ValueError('ambiguous type-only relation')
            pairs = tuple(sorted(set(tuple(p) for p in pairs)))
            forward = [set() for _ in range(counts[a])]
            reverse = [set() for _ in range(counts[b])]
            for u,v in pairs:
                if type(u) is not int or type(v) is not int or not 0 <= u < counts[a] or not 0 <= v < counts[b]:
                    raise ValueError('invalid original edge endpoint')
                forward[u].add(v)
                reverse[v].add(u)
            forward,reverse = tuple(tuple(sorted(r)) for r in forward),tuple(tuple(sorted(r)) for r in reverse)
            directions[a,b] = _direction(a,b,forward,reverse)
            directions[b,a] = _direction(b,a,reverse,forward)
            canonical.append((a,b,pairs))
        self.counts = counts
        self.relations = tuple(canonical)
        self.directions = MappingProxyType(directions)

    def fingerprint(self):
        payload = repr((self.counts,self.relations,tuple(sorted(self.directions.items()))))
        return hashlib.sha256(payload.encode('utf-8')).hexdigest()
