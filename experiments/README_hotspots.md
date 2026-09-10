# Repeated-neighborhood work diagnosis (server only)

Sync `src/`, `experiments/`, and `CMakeLists.txt`, then run on Linux:

```bash
cd ~/Desktop/second-work
python3 -B experiments/run_server_hotspots.py
```

This is NOT another performance sweep or an algorithm replacement. It executes
instrumented mode 11 once for IMDB large (movie-actor-movie, epsilon 0.5, mu 5)
and once for Foursquare (user-venue-user, epsilon 0.5, mu 5). Yelp is not in this
focused round. No raw conversion, offline build, or HINSCAN run is performed.

Keep the uncompressed `server-results/*-anchor-*` directory from the preceding
experiment on the server. The runner selects the newest complete anchor_v1 run
containing both base.bri files, the manifest, offline.tsv and control hashes.
It verifies index SHA256 before and after querying, and compares both clusters
and roles with the verified mode 11 reference. It does not modify reused indices.
The exported small tar.gz alone is insufficient because it contains no indices.

Return the printed `server-results/*-hotspots-*.tar.gz`. Check status.json first.
No speedup is reported: per-row instrumentation changes runtime and memory, and
the coverage study is additional POST-query work. Do not compare its elapsed
time with uninstrumented algorithms or add it to prior online benchmark results.

## What is measured

- In diagnostic builds only, raw posting visits and existing timer intervals are
  attributed to canonical neighborhood rows. Identical factor lists already
  share rows; these are not counts of distinct original vertices.
- Full neighborhood generation and uncached streaming are counted separately.
  Their raw visits are disjoint and sum to `scan_entries`.
- `top_1/10/100/1000_rows_work_share` measures concentration by raw visits,
  not time. `repeated_row_work_share` includes work on rows with more than one
  generation/stream call; it is NOT the fraction provably removable by caching.
- `one_full_expansion_per_working_row_entries` sums the cost in raw posting
  visits of generating each working canonical row once. Its ratio to current
  visits is a work reference, NOT a runtime or memory-feasibility prediction.
- Only the hottest 512 rows receive detailed output. Their combined fraction
  of total work is `reported_work_share`.
- For those rows, the 1, 2, and 4 largest individual witness postings are unioned
  with exact deduplication. This is NOT an optimal maximum-coverage selection.
  `any4_union_upper` is the degree-capped sum of the four largest posting sizes,
  a valid upper bound for ANY choice of at most four witnesses.
- Coverage traversal has a 50-million-posting-entry diagnostic cap. Rows that
  would exceed the remaining allowance have `coverage_complete=0` and blank
  exact-coverage fields. Other rows may still be inspected. This cap does not
  affect the clustering algorithm or any output roles.
- Weighted coverage uses raw visit counts on the fully inspected rows only;
  `coverage_work_share` states their fraction of ALL measured work. A small
  fraction must not be generalized to the entire dataset.
- `list_or_dense_payload_bytes` is a representation-size estimate per full row,
  excluding headers and sparse bitmap alternatives. It is not peak memory.
- The trace costs one 64-byte record per target vertex on the current 64-bit
  build. Reporting also uses row IDs, timestamps and temporary witness lists.
  Process peak RSS includes this diagnostic overhead.

The diagnostic requires no new clustering parameters. Existing BUILD_JOBS,
BUILD_DIR, RESULT_ROOT and LIMIT_SECONDS environment overrides are operational
settings; normal use needs none. Script tests and small independent graph tests
run before the two actual queries. Large datasets are never run on the local PC.
