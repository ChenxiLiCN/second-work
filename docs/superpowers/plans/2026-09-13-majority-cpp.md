# Majority C++ Integration Implementation Plan

> **For agentic workers:** Use test-driven development and review before completion. User explicitly requests continuous execution without intermediate approval prompts; implementation domains may proceed in parallel only with disjoint file ownership.

**Goal:** Deliver the approved algorithm as independently callable C++ build/query tools plus a one-run server script.

**Architecture:** Existing base.bri remains unchanged; groups.mgi stores only original-relation CSR majority groups. Layered completion precedes selected-source FactorIndex construction; existing PSCAN CoreConnectivity handles residual whole components. Server runner executes the three approved datasets once per existing query configuration.

**Tech Stack:** C++17, existing CMake/MinGW and Linux g++, Python standard-library tests.

**Spec:** docs/LAYERED_GROUP_PROTOTYPE.md and docs/superpowers/plans/2026-09-13-layered-group-prototype.md, with C++ integration obligations made concrete below.

## Global Constraints

- Offline graph/schema only; no path/epsilon/mu or projected pair table.
- Keep PSCAN implementation and existing CLI inputs/behavior unchanged; new CLI needs no tuning knobs.
- Paper mu counts self; upstream executable must receive mu-1 exactly once.
- Finish on authorized current checkout; preserve existing unrelated changes. No commits/pushes, real-data local runs, or intermediate opinion requests.
- Online compute includes all planner/factor/scanner/merge work, excludes index load and output writes. Offline excludes raw reads/writes but includes adjacency, roundtrip metadata and majority construction.
- Raw/index integrity validation belongs to load or measured compute, never silently hidden as free work.
- Same-type directed transitions ineligible for certification retain existing exact path. Invalid input must not fast-complete.

## Interfaces and ownership

- Offline worker creates src/index/MajorityIndex.h/.cpp, src/tools/mgi_build_index.cpp and src/tools/verify_majority_index.cpp. `MajorityDirection` contains uint64 offsets and VertexId members. `MajorityIndex::build(graph)`, `load(path,graph)`, `save(path,graph)`, `groups_for(graph,transition)` bind groups to the raw graph and validate stored data.
- Online worker creates src/scan/LayeredCompletion.h/.cpp and src/tools/mgi_query_index.cpp. `run_layered_completion(graph,groups,path,threshold,other_mu)` returns clustering and phase counts/timings; source IDs are restored after residual scanning.
- Root modifies src/index/FactorIndex.h/.cpp with `build_selected(graph,path,sorted_sources,prepare_exact=true)`. Nonselected sources are not expanded; original intermediates are never deleted. Full selection reuses old fast paths. Empty/subset selection and invalid selections need direct verification. Root creates CLI audit and CMake targets.
- Runner worker creates experiments/run_server_majority.py and experiments/test_server_majority.py; no production files. Existing raw-data lossless adapters reused. New result directory; no historical timing substitution.

## Verification-led steps

- [ ] Add experiments/audit_majority_cpp.py and run it before new tools exist: expected explicit missing-executable failure. Reference computes independent full closed neighborhoods only on generated tiny graphs.
- [ ] Add direct selected-factor verifier before API implementation; compile should fail for missing build_selected. Verify sorted original-ID subset has correct induced projected neighborhoods and less source expansion than whole build, including a repeated-type half path passing through nonselected original vertices.
- [x] Implement index CSR ancestor groups, graph binding/checksum and no-overwrite builder. Check serialization roundtrip, corruption rejection and member linear bound with tiny verifier.
- [x] Implement layered planner and exact residual PSCAN merge. Test early and scalar completion actually skip factor work, mixed completed/residual roles, reverse targets, empty domains, ghost bridges and randomized cyclic schemas.
- [x] Build only required CMake targets in a separate build directory; run audit, old/new Python models and runner tests. No local real data.
- [ ] Run independent review, resolve substantive findings and repeat covering tests. Preserve explicit limitations of reconstructed HINSCAN baseline and lack of server measurements.
- [x] Record changes and simple Linux command in docs/MAJORITY_CPP_SERVER.md.

2026-09-13 recovery acceptance: missing four C++ files now created; fresh build-majority-verified compiles. Both C++ verifiers pass, 127 CLI oracle queries pass, 32 model tests and 5 runner tests pass, existing verify_adaptive_fli reports cluster_cases=64800 and all_passed=1. Full final independent reviewer approval has not been obtained; local code inspection and independent semantic differential testing are complete. Initial red-test history from the interrupted implementation is not reconstructed or marked complete here. No Linux real-data result is claimed.

Core acceptance in the black-box audit is concretely:

```python
assert read_result(output / f'result-{eps}-{mu}.txt', n) == cluster_part(reference(rows, eps, mu))
assert read_roles(output / f'roles-{eps}-{mu}.txt', n)['roles'] == reference(rows, eps, mu)['roles']
assert completed_core + completed_noncore + residual == n
assert residual == 0 and residual_half_expansion_entries == 0  # designated complete cases
```

Command contracts:

```bash
mgi_build_index HIN_DIR NEW_INDEX_DIR
mgi_query_index INDEX_DIR META_PATH EPSILON MU OUTPUT_DIR
python3 -B experiments/audit_majority_cpp.py --bin-dir BUILD/bin
python3 -B experiments/run_server_majority.py
```

The old baseline reconstruction materializes the query graph and invokes upstream PSCAN; it does not independently produce the paper's full roles. Report this asymmetry rather than call it an official complete HINSCAN implementation. Full indexed roles are verified against independent tiny semantics and current production control.
