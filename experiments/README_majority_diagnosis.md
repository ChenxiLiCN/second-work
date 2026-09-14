# Majority residual-work diagnosis

## Run on Linux

Apply the accompanying source update in the existing server project, then:

```bash
python3 -B experiments/run_server_majority_diagnosis.py
```

The runner automatically selects the newest complete `majority_v1` run with
retained `index/base.bri` and `index/groups.mgi` for all three datasets. Keep the
original unpacked server run. The small result archive contains no indices.
To select a particular run:

```bash
SOURCE_RUN="$PWD/server-results/20260913-214117-majority-_6d3yesl" python3 -B experiments/run_server_majority_diagnosis.py
```

It builds the query tools, runs protocol tests and the independent 127-query
synthetic oracle, then runs control and Majority once each on Yelp, IMDB large
and Foursquare. The real-data indices are reused read-only; no real-data
normalization, offline build or materialized baseline rerun is performed.
The tiny oracle does build synthetic indices and projections.

Return the printed `*-majority-diagnosis-*.tar.gz`. A successful `status.json`
has `all_passed: true` and `query_pairs: 3`. Failure still produces an archive.
Both query outputs must match the previous verified cluster AND role hashes;
both input indices are hashed before and after the pair. A missing/invalid
counter, wrong semantics, or wrong scan mode invalidates the comparison.

## Reading the report

`comparison.tsv` contains one row per dataset and metric. `majority_to_control`
is Majority divided by the freshly run control; zero control yields a blank
ratio. Counts remain integer values. Timings are single observations, not
stable speedup estimates. Both algorithms use mode 9 and self-inclusive mu=5.

- Compare candidate visits, exact checks, witness reads, generated entries and
  streamed entries separately. They are different work categories, not an
  additive estimate of total runtime.
- The `residual_` prefix denotes work on the remaining factor graph. Majority
  proof work is separately available in `majority.log`, including member and
  raw-relation reads. Total online compute includes proof, preparation, scan
  and merge, but excludes index loading and result writing.
- `role_ms` is inside `noncore_ms`; cache timers are inside scan phases. Do not
  sum all timer rows. Raw scan counters are retained through result merging;
  the unprefixed core/cluster totals include early-completed components.
- `residual_used_roundtrip_metadata`, degree recomputation, posting sorting,
  projected-edge count and half-path incidences distinguish preparation cost
  from scan cost. All scan/factor counters are zero if nothing remains.

## Evidence reviewed on 2026-09-14

| Archive | Observation | Scope |
| --- | --- | --- |
| `20260913-214117-majority-_6d3yesl` | Online 16.638 / 27.511 / 10.283 s versus control 16.677 / 26.113 / 9.556 s, in Yelp / IMDB / Foursquare order | One run, mode 9, all output checks passed |
| `20260910-222419-hotspots-b_a4s3jh` | IMDB/Foursquare raw visit amplification 35.84 / 11.97 versus expanding each working row once; top 1000 rows cover only 8.52% / 19.87% of work | Mode 11 diagnostic; not evidence of achievable time savings in mode 9 |
| `20260911-111323-shared-groups-3btafmxx` | Proof covers 8.92% / 16.63% of checks; potential predicate balance -1825 / -49 ms | Hindsight diagnostic; no demonstrated net benefit, not an executable online speedup |
| `20260912-171914-containment-6h1pp2xv` | All three structural builds complete; Yelp has no parent edges in any raw-relation view | No online query or speedup measured; raw relation containment does not establish query usefulness |

Source inspection found `FactorIndex::build_impl` only reuses two-hop metadata
when `sources == nullptr`. Majority calls `build_selected` on the residual
sources, so a proper subset recomputes degrees and posting order. This explains
an additional preparation cost, not the full scan-time result. No algorithm
change is made in this diagnostic update.

The leading hypothesis is that completed vertices account for little expensive
scan work. It remains unconfirmed until current mode-9 counters are compared.
If retained candidate/check/witness work stays near control, prioritize reducing
that work. If it drops sharply without runtime improvement, investigate memory
access and preparation costs before choosing a new algorithm.

## Local verification

Only synthetic data is used locally. This worktree does not contain the embedded
upstream pSCAN source; for local validation its three translation units were
compiled from the existing desktop checkout into the worktree build directory.
All modified project targets were freshly compiled from this worktree. The
server runner expects the existing server checkout to retain `third_party/pscan`.
