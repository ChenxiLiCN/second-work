#!/usr/bin/env python3
"""Fixed synthetic operation-count probe (12/24/48 targets); no files or timing."""
import json

from audit_semantics import full_path_rows, incidence, reference
from block_candidate_model import BlockQuery, RawBlockIndex


def projection_reads(counts, relations, path):
    """Independent per-source traversal count, NOT the optimized HINSCAN timer."""
    transitions = {}
    for a, b, edges in relations:
        forward, reverse = [set() for _ in range(counts[a])], [set() for _ in range(counts[b])]
        for u, v in edges:
            forward[u].add(v)
            reverse[v].add(u)
        transitions[a, b], transitions[b, a] = forward, reverse
    types = [ord(part)-ord("A") for part in path.split("-")]
    reads = 0
    for u in range(counts[types[0]]):
        frontier = {u}
        for a, b in zip(types, types[1:]):
            following = set()
            for v in frontier:
                reads += len(transitions[a, b][v])
                following.update(transitions[a, b][v])
            frontier = following
    return reads


def run_probe():
    report = dict(timing_benchmark=False, all_matched=True,
                  scope=("Synthetic symmetric A-Z type paths, distinct adjacent types, "
                         "one unlabelled relation per unordered type pair and synthesized inverses; "
                         "root-summary model, not full index. These nine probes use A-B-A only."),
                  memory_limitation="Online pending positive pairs can form the entire similarity graph.",
                  comparison="Reference reads are per-source traversal only, not full HINSCAN cost.",
                  cases=[])
    for family in ("overlap_three_witnesses", "matching", "cycle"):
        for n in (12, 24, 48):
            if family == "overlap_three_witnesses":
                counts, relations = [n, 3], [(0, 1, [(u, w) for u in range(n)
                                                     for w in range(3) if w != u % 3])]
                epsilon, mu = "0.9", 5
            elif family == "matching":
                counts, relations = incidence(n, [(u, n//2+u) for u in range(n//2)])
                epsilon, mu = "1", 2
            else:
                counts, relations = incidence(n, [(u, (u+1) % n) for u in range(n)])
                epsilon, mu = "0.5", 3
            path = "A-B-A"
            rows = full_path_rows(counts, relations, path)
            expected = reference(rows, epsilon, mu)
            engine = BlockQuery(RawBlockIndex(counts, relations), path, epsilon, mu)
            result = engine.run()
            matched = result == expected
            report["all_matched"] &= matched
            s = engine.stats
            reads = {phase: getattr(s, phase+"_reads")
                     for phase in ("degree", "candidate", "proof", "exact", "role")}
            report["cases"].append(dict(
                family=family, n=n, epsilon=epsilon, mu=mu, matched=matched,
                original_edges=sum(len(set(edges)) for _, _, edges in relations),
                projected_edges=sum(len(row)-1 for row in rows)//2,
                reference_projection_reads=projection_reads(counts, relations, path),
                model_original_reads=sum(reads.values()), reads_by_phase=reads,
                source_blocks=len(s.source_builds), candidate_endpoints=s.candidate_endpoints,
                pair_checks=s.pair_checks, similar_blocks=s.similar_blocks,
                root_summary_hits=s.summary_hits, proof_calls=s.proof_calls,
                clique_fastpaths=s.clique_fastpaths, pending_created=s.pending_created,
                pending_peak=s.pending_peak, core_skip_checks=s.core_skip_checks,
                vertex_updates=s.vertex_updates, mask_items=s.mask_items, sort_items=s.sort_items))
    return report


if __name__ == "__main__":
    report = run_probe()
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report["all_matched"] else 1)
