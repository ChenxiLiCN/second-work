#!/usr/bin/env bash
# Linux server runner: build | smoke | medium | large | all
# Optional variables: DATA_ROOT, REPEATS, LIMIT_SECONDS, BUILD_JOBS,
# RUN_HINSCAN_BASELINE=0/1, KEEP_PROJECTED=0/1, RESULT_ROOT.
set -uo pipefail

MODE="${1:-smoke}"
case "$MODE" in build|smoke|medium|large|all);; *) echo "Usage: $0 {build|smoke|medium|large|all}"; exit 2;; esac
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DATA_ROOT="${DATA_ROOT:-$ROOT/data}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-linux}"
RESULT_ROOT="${RESULT_ROOT:-$ROOT/server-results}"
REPEATS="${REPEATS:-3}"
LIMIT_SECONDS="${LIMIT_SECONDS:-7200}"
BUILD_JOBS="${BUILD_JOBS:-4}"
RUN_HINSCAN_BASELINE="${RUN_HINSCAN_BASELINE:-1}"
KEEP_PROJECTED="${KEEP_PROJECTED:-0}"
BIN="$BUILD_DIR/bin"
RUN_DIR="$RESULT_ROOT/$(date +%Y%m%d-%H%M%S)-$MODE"
RUNS="$RUN_DIR/runs.tsv"
OFFLINE="$RUN_DIR/offline-index.tsv"

[[ "$REPEATS" =~ ^[1-9][0-9]*$ ]] || exit 2
[[ "$LIMIT_SECONDS" =~ ^[1-9][0-9]*$ ]] || exit 2
mkdir -p "$RUN_DIR"
printf 'dataset\tcase\trepeat\tmode\tstatus\telapsed_s\tmax_rss_kb\n' > "$RUNS"
printf 'dataset\trepeat\tstatus\telapsed_s\tmax_rss_kb\tindex_bytes\tsha256\n' > "$OFFLINE"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing command: $1"; exit 1; }; }

measure() {
  local prefix="$1" code; shift
  set +e
  /usr/bin/time -f 'elapsed_s=%e\nmax_rss_kb=%M\ncommand_exit=%x' -o "${prefix}.time" \
    timeout --kill-after=30s "${LIMIT_SECONDS}s" "$@" > "${prefix}.log" 2>&1
  code=$?
  set -e
  STATUS="$code"
  ELAPSED="$(awk -F= '$1=="elapsed_s"{print $2}' "${prefix}.time" | tail -n1)"; ELAPSED="${ELAPSED:-NA}"
  RSS="$(awk -F= '$1=="max_rss_kb"{print $2}' "${prefix}.time" | tail -n1)"; RSS="${RSS:-NA}"
}

record() {
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$STATUS" "$ELAPSED" "$RSS" >> "$RUNS"
}

build_programs() {
  for tool in cmake g++ timeout awk sha256sum; do need "$tool"; done
  test -x /usr/bin/time || { echo "Install the Linux time package"; exit 1; }
  test -f "$ROOT/third_party/pscan/Graph.cpp" || { echo "Upload third_party/pscan first"; exit 1; }
  cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=g++ -DBUILD_PSCAN_BASELINE=ON > "$RUN_DIR/cmake-configure.log" 2>&1
  cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS" --target \
    bri_build_index hin_materialize pscan_baseline \
    bri_query_witness_exclusion verify_adaptive_fli > "$RUN_DIR/cmake-build.log" 2>&1
}

build_index() {
  local name="$1" raw="$2" dir="$RUN_DIR/indexes/$1" bri first="" hash bytes r
  test -f "$raw/base.txt" || { echo "Dataset missing: $raw"; return 1; }
  mkdir -p "$dir"; bri="$dir/base.bri"
  for ((r=1;r<=REPEATS;r++)); do
    echo "[$name] offline index $r/$REPEATS"
    measure "$dir/build-$r" "$BIN/bri_build_index" "$raw" "$bri"
    if [[ "$STATUS" == 0 ]]; then
      hash="$(sha256sum "$bri" | awk '{print $1}')"; bytes="$(stat -c '%s' "$bri")"
      [[ -z "$first" ]] && first="$hash"
      [[ "$first" == "$hash" ]] || { echo "BRI hash mismatch: $name"; return 1; }
    else hash=NA; bytes=NA; fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$r" "$STATUS" "$ELAPSED" "$RSS" "$bytes" "$hash" >> "$OFFLINE"
    [[ "$STATUS" == 0 ]] || return 1
  done
}

run_case() {
  local name="$1" raw="$2" path="$3" eps="$4" mu="$5"
  local bri="$RUN_DIR/indexes/$name/base.bri" case="${path}_${eps}_${mu}" dir r projected
  local baseline current mat_s mat_rss scan_s scan_rss before after
  dir="$RUN_DIR/cases/$name/$case"; mkdir -p "$dir/current"
  before="$(sha256sum "$bri" | awk '{print $1}')"
  for ((r=1;r<=REPEATS;r++)); do
    echo "[$name $case] repeat $r/$REPEATS"
    baseline=0; current=0; projected="$dir/projected-$r"
    if [[ "$RUN_HINSCAN_BASELINE" == 1 ]]; then
      measure "$dir/materialize-$r" "$BIN/hin_materialize" "$raw" "$path" "$projected"
      record "$name" "$case" "$r" materialize; mat_s="$ELAPSED"; mat_rss="$RSS"
      if [[ "$STATUS" == 0 ]]; then
        measure "$dir/pscan-$r" "$BIN/pscan_baseline" "$projected" "$eps" "$mu" output
        record "$name" "$case" "$r" pscan; scan_s="$ELAPSED"; scan_rss="$RSS"
        if [[ "$STATUS" == 0 ]]; then
          baseline=1; STATUS=0
          ELAPSED="$(awk -v a="$mat_s" -v b="$scan_s" 'BEGIN{printf "%.6f",a+b}')"
          RSS="$(awk -v a="$mat_rss" -v b="$scan_rss" 'BEGIN{print a>b?a:b}')"
          record "$name" "$case" "$r" hinscan_complete
        fi
      fi
    fi
    measure "$dir/current-$r" "$BIN/bri_query_witness_exclusion" "$bri" "$path" "$eps" "$mu" "$dir/current"
    record "$name" "$case" "$r" indexed_online
    [[ "$STATUS" == 0 ]] && current=1
    if [[ "$baseline" == 1 && "$current" == 1 ]]; then
      cmp "$projected/result-$eps-$mu.txt" "$dir/current/result-$eps-$mu.txt" || { echo "Cluster mismatch"; return 1; }
    fi
    printf 'repeat=%s baseline_ok=%s indexed_ok=%s\n' "$r" "$baseline" "$current" >> "$dir/status.txt"
    if [[ "$KEEP_PROJECTED" != 1 && "$projected" == "$dir"/projected-* ]]; then rm -rf -- "$projected"; fi
  done
  after="$(sha256sum "$bri" | awk '{print $1}')"; [[ "$before" == "$after" ]] || { echo "BRI changed"; return 1; }
}

run_dataset() { build_index "$1" "$2" && run_case "$1" "$2" "$3" "$4" "$5"; }

summarize() {
  command -v python3 >/dev/null 2>&1 || return 0
  python3 - "$RUNS" "$RUN_DIR/summary.tsv" <<'PY'
import csv, statistics, sys
groups={}
with open(sys.argv[1],encoding='utf-8') as f:
    for r in csv.DictReader(f,delimiter='\t'):
        if r['status']=='0' and r['elapsed_s']!='NA':
            groups.setdefault((r['dataset'],r['case'],r['mode']),[]).append(float(r['elapsed_s']))
with open(sys.argv[2],'w',newline='',encoding='utf-8') as f:
    w=csv.writer(f,delimiter='\t'); w.writerow(('dataset','case','mode','n','median_s','min_s','max_s'))
    for key in sorted(groups):
        values=groups[key]
        w.writerow((*key,len(values),f'{statistics.median(values):.6f}',f'{min(values):.6f}',f'{max(values):.6f}'))
PY
}

set -e
{
  date -Is; uname -a; lscpu 2>/dev/null || true; free -h 2>/dev/null || true
  g++ --version 2>/dev/null || true; cmake --version 2>/dev/null || true
  git -C "$ROOT" rev-parse HEAD 2>/dev/null || true
} > "$RUN_DIR/environment.txt" 2>&1
build_programs
if [[ "$MODE" == build ]]; then echo "Build complete. Logs: $RUN_DIR"; exit 0; fi
measure "$RUN_DIR/oracle" "$BIN/verify_adaptive_fli" "$RUN_DIR/fixtures"
[[ "$STATUS" == 0 ]] && grep -q '^all_passed=1$' "$RUN_DIR/oracle.log" || { echo "Correctness oracle failed"; exit 1; }

if [[ "$MODE" == smoke || "$MODE" == all ]]; then
  run_dataset dblp_small "$DATA_ROOT/raw/hin_text/dblp_small" A-P-A 0.5 5
fi
if [[ "$MODE" == medium || "$MODE" == all ]]; then
  run_dataset imdb_legacy "$DATA_ROOT/raw/hin_text/imdb_legacy" A-M-D-M-A 0.5 5
  run_dataset yelp "$DATA_ROOT/derived/normalized/yelp" U-R-B-R-U 0.9 5
fi
if [[ "$MODE" == large || "$MODE" == all ]]; then
  run_dataset dblp_apv_full "$DATA_ROOT/raw/hin_text/dblp_apv_full" A-P-A 0.5 5
  run_dataset dblp_v18 "$DATA_ROOT/raw/hin_text/dblp_v18" A-P-A 0.5 5
fi
summarize
echo "Completed. Results: $RUN_DIR"
[[ -f "$RUN_DIR/summary.tsv" ]] && echo "Summary: $RUN_DIR/summary.tsv"
