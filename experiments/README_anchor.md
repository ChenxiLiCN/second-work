# Single-anchor server experiment

From the project root on the Linux server:

```bash
python3 -B experiments/run_server_anchor.py
```

Sync `src/`, `experiments/` and `CMakeLists.txt`. Existing raw data must be at:

- `data/raw/hin_text/yelp/`
- `data/raw/entity_relation/imdb_large/`
- `data/raw/entity_relation/foursquare/`

The script builds Release executables, runs script tests and correctness oracles,
adapts each raw dataset without sampling, builds one base index per dataset,
and runs reconstructed HINSCAN, mode 11 (control), and mode 12 (anchor) once each.
The existing selected-data script still defaults to modes 10 and 11.

| Dataset | Path | Epsilon | Mu |
|---|---|---:|---:|
| Yelp | U-R-B-R-U | 0.9 | 5 |
| IMDB large | movie-actor-movie | 0.5 | 5 |
| Foursquare | user-venue-user | 0.5 | 5 |

## Scope

This is the FIRST prototype: a rejection filter before full similarity checking.
Candidate enumeration, core counters, clustering rules and roles are unchanged.
There is no multi-anchor logic and no batch candidate skipping.
The persistent BRI format and contents are unchanged. Anchor selection and all
intersection progress records are query-local, including for long paths; they
are not presented as new offline preprocessing.

For a vertex u, choose its largest witness posting A (tie: smallest witness ID).
It is contained in the closed neighborhood. Let r_u = degree(u) - size(A).
For u,v with postings A,B and required common count tau, rejection is safe iff
the filter proves `size(A intersect B) < tau - r_u - r_v`.
Nonpositive thresholds bypass the filter. A failed filter is UNKNOWN, not Similar.
Sorted posting intersections maintain lower/upper bounds and resumable cursors.
Reversing endpoints reuses the same canonical orientation. Hash collisions evict
state, never reuse another pair's bounds. Repeated path witnesses are deduplicated
by the existing FactorIndex; isolated vertices bypass the filter.

Both variants keep the existing 32 MiB neighborhood-cache budget. Mode 12 adds
one 32-bit anchor ID per target vertex and at most 65,536 32-byte progress records
(2 MiB). The bound is internal, not a new query parameter. These extra allocations
are reported; they are not counted as disk index bytes. Local oracle budgets
include zero, one record, and small eviction-heavy tables.

## Results and timings

Return the printed `server-results/*-anchor-*.tar.gz`. It contains only small
logs, timing records, metadata and tables, not normalized data, indices or graphs.
The unpacked normalized data and base indices remain on the server for auditing.
Transient baseline projected graphs and query output files are removed after hashing.
Original raw inputs are never modified. Yelp's known two domain-declaration
corrections and IMDB's sparse-ID remapping remain recorded in `data-audit.json`.

- `summary.tsv`: HINSCAN time, offline compute time, online compute time, index
  bytes, speedup, total time difference from mode 11, and anchor diagnostics.
- `runs.tsv`: full counters and process peak RSS.
- `status.json`: validity and failures. Failed correctness/configuration runs do
  not produce valid speedups. Baseline is reconstructed flow, not official HINSCAN.
- Offline compute excludes input buffering/parsing and index writing; its scope
  is adjacency allocation, population, sorting and deduplication.
- Online compute is factor preparation plus the entire query call, including
  anchor selection, progress-table allocation, filtering and timer overhead.
  Reading the index and writing results are excluded.
- `anchor_filter_ms` covers every filter call, including unsuccessful calls;
  `anchor_prepare_ms` covers anchor construction. These are nested parts of
  online time: do not add them to `online_compute_s` again.
- `anchor_eligible`: positive-threshold calls, not successful rejections.
- `anchor_cache_hits`: state reuse, not necessarily avoided scans; inspect
  `anchor_resumed`, `anchor_cached_rejects`, and `anchor_entries_advanced` too.
- `full_checks_avoided_vs_control` is the difference in full predicate counts.
  The exact counterfactual time of each avoided predicate is NOT measured.
  Use `online_saved_vs_control_s` for the net time difference (one run only).

Build jobs default to 4 and subprocess timeout to 7200 seconds. Existing optional
environment settings DATA_ROOT, BUILD_DIR, RESULT_ROOT, BUILD_JOBS and
LIMIT_SECONDS remain available for deployment; no algorithm tuning flags added.
