# Layered Majority-Group Prototype Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended for independent tasks) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** Retain an independently testable implementation of the approved majority-group certificates and whole-component completion, without changing production PSCAN.

**Architecture:** Query-independent directed original relations own dyadic strict-majority groups. A layered query planner returns completed whole projection components and unresolved whole components. A test-only adapter merges independent exact residual results; it is not a production PSCAN adapter.

**Tech Stack:** Python standard library, unittest, exact Fraction arithmetic; C++ integration is outside this plan.

**Spec:** The algorithm contract below records the approved continuation; older block-model documents are historical, not the implementation specification.

## Global Constraints

- Offline knows only graph and schema, not the query path, epsilon or mu.
- Do not store projected edge sets or pair similarities in the offline index.
- Preserve non-independent HINSCAN clusters and vertex roles; external mu includes self and is at least 2.
- Time efficiency precedes index compression; do not introduce user tuning parameters.
- No local real datasets, server experiments, production changes, commits or pushes in this prototype task.
- Count certificate preprocessing honestly; do not report operation ratios as HINSCAN speedups.
- User explicitly authorized adding independent files directly on main on 2026-09-13. Preserve all pre-existing untracked files.

## Algorithm contract

For each original directed relation R:B->C, pad C to a power-of-two leaf domain. Store G_Z={b:2*|N(b) intersect Z|>|Z|} for each nonempty dyadic interval group. Deduplicate raw edges. Build by accumulating row counts at ancestors of actual neighbors, not scanning every row against every interval. Total member records per direction are at most sum_b max(0,2*d_b-1). Each group is an inner roundtrip clique. Store existing exact roundtrip closed degrees as scalars, never the roundtrip rows. The reference builder may enumerate one temporary row at a time; count that work separately.

Scope of this isolated model: A-Z types; one original relation per unordered pair of distinct types; implicit reverse transitions; symmetric type paths of positive even edge length. Explicitly reject unsupported inputs rather than returning incorrect completed results. Production must retain its existing handling outside this scope.

Let P=Q R reverse(R) reverse(Q), H=Q R. Build the H layered graph, marking forward reachability from all targets and backward reachability to terminals. Union only edges on complete half paths. These target components equal projection components; inactive targets are singletons. Complete size<mu components as all outlier.

If a group covers every active penultimate vertex of a component, that component is a clique; complete it before scalar-profile work. For remaining components calculate U_P=min(component_size,max(1,capped_full_path_walk_count)); for Q empty reuse offline exact closed degrees instead. Calculate lower preimage counts ell_b by indegree followed by max propagation along Q, upper Q walk counts U_Q, and maximum target U_P propagated along Q. For group G in component C set delta=max_{u in C} U_Q(u), L=max(max ell_b,ceil(sum ell_b/min(|G|,delta))), M=max propagated U_P. Accept if L>=mu and L*epsilon.denominator>=M*epsilon.numerator.

Batch all accepted group lifts using Q layered edges: trim forward-unreachable states and states unable to reach an accepted terminal; attach only forward-reachable terminal members to group nodes. Never construct one lifted target list per group in the planner. All active targets are certified core. Complete a remaining projection component only when all its targets are certified and connected in this certificate DSU. All other whole components are residual; never split one for fallback.

Planner output is `Plan(completed, residual_components, stats)`: completed maps original target ID to role and canonical cluster ID (or no cluster for outlier); residual components are sorted tuples of original target IDs. Instrumentation counts raw adjacency entries, group member visits, and DSU calls by phase, not time estimates. An optional audit callback receives accepted inner groups and scalar bounds only, never lifted target lists.

## File map

- Create experiments/majority_group_index.py: immutable raw relation storage, offline groups and degree scalars.
- Create experiments/layered_group_model.py: layered planner and operation counters; no reference import or projected rows.
- Create experiments/test_layered_group_model.py: tests and explicitly test-only exact residual merge.
- Create experiments/measure_layered_group_model.py: fixed tiny operation probes, JSON stdout only.
- Create docs/LAYERED_GROUP_PROTOTYPE.md: verified results, limits, changes and C++ follow-up.

## Task 1: One tightly coupled prototype and verification cycle

**Interfaces:** `MajorityIndex(counts, relations)`, `index.directions[(a,b)].groups`, `index.fingerprint()`, `LayeredPlanner(index,path,epsilon,mu).run()` returning Plan. Test adapter `merge_reference(index,path,epsilon,mu,plan)` is never imported by the planner.

- [x] Write behavioral tests before implementation. Minimum explicit example:

```python
index = MajorityIndex([4, 4], [(0, 1, [(u,b) for u in range(4) for b in range(4) if b != u])])
plan = LayeredPlanner(index, 'A-B-A', '1', 4).run()
assert not plan.residual_components
assert all(plan.completed[u] == ('core', 0) for u in range(4))
```

- [x] Run `python -B -m unittest discover -s experiments -p test_layered_group_model.py -v`; confirm the missing module failure.
- [x] Implement ancestor accumulation; a row with neighbors {0,1,2} in an eight-leaf domain emits its three leaves, [0,2), and [0,4), but not [0,8). Check the 2*d-1 bound and clique property exhaustively for all 4x3 relations.
- [x] Implement trim-and-union with a distinct node per (layer,vertex). Test the ghost case u->b0, v->b1 and groups {b0,b2},{b1,b2}: unreachable b2 must not merge u and v.
- [x] Implement early component completion, scalar bounds and accepted-group connectivity. Use exact integer arithmetic:

```python
lower = max(maximum_preimage_lower, (sum_preimage_lower + divisor - 1) // divisor)
accepted = lower >= mu and lower * epsilon.denominator >= upper * epsilon.numerator
```

- [x] Implement the test-only merge on induced residual components, remapping canonical cluster IDs back to original IDs. Compare full cores, memberships and roles to audit_semantics.reference on full_path_rows.
- [x] Verify eight existing semantic fixtures, empty types, reverse target queries, epsilon/mu boundaries, repeated-type long paths, duplicate edges, vertex relabeling, offline fingerprint stability and randomized paths of 2/4/6/8 edges. Audit every accepted group's actual lift against explicit neighborhoods in tests only.
- [x] Run fixed n=12/24/48 dense-overlap, matching and cycle probes. Report all planner entry visits and residual source counts; no HINSCAN runtime claim.
- [x] Run old and new independent test suites, inspect changes, and record exact measured test/probe results and all new filenames. Result: 32 tests (18 new, 14 old) and 16 JSON probes pass. See docs/LAYERED_GROUP_PROTOTYPE.md. No production speed claim until C++ integration and server measurements.
- [ ] Obtain a complete final independent review before C++ integration. Preliminary review found no defect in the core bounds/trimming; reviewer session ended before final assessment. This is not marked as final approval.

## Review and handoff

This is a single tightly coupled model/test deliverable, executed inline. The offline/online interface above and the independent-reference boundary are shared invariants, not separate parallel implementation tasks. No existing file is modified by this plan. A fresh read-only review of the completed algorithm is appropriate before C++ integration.

For C++: add original-relation groups to the offline format; invoke this planner before FactorIndex construction; build factors only for retained original source IDs; keep PSCAN unchanged and merge canonical results. Online timing must include planner, allocations/copies, residual factor build, PSCAN and merge, excluding index reads and writes. Measure offline original-index construction separately, excluding raw reads and writes.
