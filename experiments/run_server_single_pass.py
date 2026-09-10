#!/usr/bin/env python3
"""One pass per DBLP: single-pass predicate versus unchanged mode 9."""
import csv
import json
import os
from pathlib import Path
import re
import tarfile
import tempfile
import time
from run_server_ablation import ROOT, checked, digest, fields, measured
from run_server_core import save_json, write_table, output_hashes

DATASETS = ('dblp_apv_full', 'dblp_v18')

def select_source(root):
    for p in sorted(root.glob('*-core-*'), reverse=True):
        if p.is_dir() and (p / 'offline.tsv').is_file() and all(
            (p / n / 'legacy.bri').is_file() and
            (p / n / 'combined-hashes.json').is_file() for n in DATASETS):
            return p
    raise RuntimeError('Previous core result directory with both legacy.bri files required. No index rebuild attempted.')

def historical_baseline(case):
    # Historical compute-only timing; never compare indexed compute to process wall time.
    materialize = fields(case / 'materialize.log')
    text = (case / 'pscan.log').read_text()
    match = re.search(r'Total time without IO:\s*(\d+)', text)
    meta = json.loads((case / 'baseline.json').read_text())
    if not match or meta['materialize']['status'] != 0 or meta['pscan']['status'] != 0:
        raise RuntimeError('Historical baseline missing or failed')
    return float(materialize['materialize_ms']) / 1000 + int(match[1]) / 1000000

def main():
    root = Path(os.environ.get('RESULT_ROOT', ROOT / 'server-results')).resolve()
    search = Path(os.environ.get('INDEX_SEARCH_ROOT', root)).resolve()
    build = Path(os.environ.get('BUILD_DIR', ROOT / 'build-linux-core')).resolve()
    jobs, limit = int(os.environ.get('BUILD_JOBS', '4')), int(os.environ.get('LIMIT_SECONDS', '7200'))
    if min(jobs, limit) < 1:
        raise ValueError('BUILD_JOBS and LIMIT_SECONDS must be positive')
    root.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-single-pass-'), dir=root))
    print('Results:', run, flush=True)
    rows, errors = [], []
    try:
        source = select_source(search)
        print('Reusing BASIC indexes:', source, flush=True)
        with (source / 'offline.tsv').open(newline='', encoding='utf-8') as f:
            offline = list(csv.DictReader(f, delimiter='\t'))
        inputs = {}
        for name in DATASETS:
            case = source / name
            index = case / 'legacy.bri'
            records = [r for r in offline if r['dataset'] == name and r['index'] == 'legacy']
            sha = digest(index)
            if len(records) != 1 or records[0]['status'] != '0' or records[0]['sha256'] != sha:
                raise RuntimeError(f'Index hash/build record mismatch: {name}')
            expected = json.loads((case / 'combined-hashes.json').read_text())
            baseline_meta = json.loads((case / 'baseline.json').read_text())
            if expected['result'] != baseline_meta['cluster_sha256']:
                raise RuntimeError(f'Historical baseline/reference mismatch: {name}')
            inputs[name] = dict(path=str(index), sha256=sha, bytes=index.stat().st_size,
                expected=expected, historical_hinscan_compute_s=historical_baseline(case))
        env = dict(version='single_pass_v1', source=str(source), inputs=inputs,
            repeats=1, cache_mib=32, core_certificate_mode=9,
            timing='All online_compute_s exclude index input and result output. Historical HINSCAN = materialize_ms + upstream pSCAN time without IO; not rerun in this experiment.',
            isolation='Both current queries read the SAME basic index. No enhanced degrees, new persistent index, profiler or parameter sweep.',
            predicate_policy='Cached right rows use direct intersection. Uncached rows fuse union and intersection; finish for admission when the row fits free cache, otherwise permit early termination. Partial rows are never admitted as complete.',
            counters='predicate_raw_posting_entries is generation + streaming + witness reads for the predicate engine only; excludes degree preparation, candidate generation and cached intersection units. It is NOT total query work or unique neighbors.',
            correctness='Clusters and roles matched to previous verified run; upstream baseline verifies clusters, not role semantics independently.')
        env['source_sha256'] = {str(p.relative_to(ROOT)): digest(p) for p in (ROOT / 'src').rglob('*')
                               if p.is_file() and p.suffix in ('.h', '.cpp')}
        env['source_sha256']['CMakeLists.txt'] = digest(ROOT / 'CMakeLists.txt')
        save_json(run / 'environment.json', env)
        checked(['cmake', '-S', ROOT, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_CXX_COMPILER=g++'], run / 'configure.log')
        checked(['cmake', '--build', build, '--parallel', jobs, '--target',
                 'bri_query_core_single_pass', 'bri_query_core_connectivity', 'verify_adaptive_fli'], run / 'build.log')
        print('Checking independent oracle', flush=True)
        with tempfile.TemporaryDirectory(prefix='oracle-', dir=run) as tmp:
            oracle = measured(run / 'oracle', [build / 'bin/verify_adaptive_fli', tmp], limit)
        if oracle['status'] != 0 or oracle.get('all_passed') != '1':
            raise RuntimeError('Independent oracle failed')
        for name in DATASETS:
            case = run / name
            case.mkdir()
            for label, exe in [('control', 'bri_query_core_connectivity'), ('single-pass', 'bri_query_core_single_pass')]:
                print(f'[{name}] {label} (once)', flush=True)
                try:
                    with tempfile.TemporaryDirectory(prefix='output-', dir=case) as tmp:
                        metric = measured(case / label, [build / 'bin' / exe, inputs[name]['path'],
                            'A-P-A', '0.5', '5', tmp], limit)
                        hashes = output_hashes(Path(tmp), '0.5') if metric['status'] == 0 else None
                    config_ok = (metric.get('cache_budget_mib') == '32' and
                        metric.get('used_roundtrip_metadata') == '0' and
                        metric.get('profile_group', '0') == '0')
                    config_ok &= (metric.get('block_mode') == '9' if label == 'control' else
                                  metric.get('block_mode') == '10' and metric.get('single_pass') == '1' and metric.get('witness_counts_built') == '0' and metric.get('witness_bound_checks') == '0')
                    valid = metric['status'] == 0 and hashes == inputs[name]['expected'] and config_ok
                    row = dict(dataset=name, variant=label, valid=int(valid), **metric)
                    if valid:
                        row['online_compute_s'] = (float(metric['on_demand_fli_build_ms']) + float(metric['query_call_ms'])) / 1000
                        row['predicate_raw_posting_entries'] = sum(int(metric[k]) for k in ('adaptive_posting_entries', 'adaptive_streaming_entries', 'witness_entries_read'))
                        row['historical_hinscan_compute_s'] = inputs[name]['historical_hinscan_compute_s']
                        if row['online_compute_s'] > 0:
                            row['speedup_vs_historical_hinscan'] = row['historical_hinscan_compute_s'] / row['online_compute_s']
                    rows.append(row)
                    save_json(case / f'{label}-hashes.json', hashes)
                    if not valid:
                        raise RuntimeError('Query failed, result mismatch or wrong executable')
                except Exception as e:
                    errors.append(f'{name}/{label}: {e}')
                    print(errors[-1], flush=True)
            pair = [r for r in rows if r['dataset'] == name and r['valid']]
            if len(pair) == 2 and any(pair[0].get(k) != pair[1].get(k) for k in ('certified_blocks', 'block_core_vertices')):
                for r in pair:
                    r['valid'] = 0
                    r.pop('online_compute_s', None)
                    r.pop('speedup_vs_historical_hinscan', None)
                errors.append(f'{name}: core block certificates changed')
            if digest(Path(inputs[name]['path'])) != inputs[name]['sha256']:
                for row in rows:
                    if row['dataset'] == name:
                        row['valid'] = 0
                        row.pop('online_compute_s', None)
                        row.pop('speedup_vs_historical_hinscan', None)
                raise RuntimeError(f'Index changed: {name}')
    except Exception as e:
        errors.append(str(e))
        print('ERROR:', e, flush=True)
    finally:
        write_table(run / 'runs.tsv', rows)
        keys = ('dataset', 'variant', 'valid', 'online_compute_s', 'historical_hinscan_compute_s',
            'speedup_vs_historical_hinscan', 'predicate_raw_posting_entries', 'on_demand_fli_build_ms',
            'query_call_ms', 'witness_counts_built', 'witness_entries_read', 'adaptive_posting_entries',
            'adaptive_streaming_entries', 'single_pass_complete', 'single_pass_partial',
            'candidate_posting_entries_read', 'exact_similarity_checks')
        write_table(run / 'summary.tsv', [{k:r.get(k, '') for k in keys} for r in rows])
        save_json(run / 'status.json', dict(all_passed=not errors, errors=errors, query_runs=len(rows)))
        archive = Path(str(run) + '.tar.gz')
        with tarfile.open(archive, 'w:gz') as tar:
            for p in sorted(run.rglob('*')):
                if p.is_file() and p.suffix in ('.json', '.tsv', '.time', '.log'):
                    tar.add(p, arcname=str(Path(run.name) / p.relative_to(run)))
        print('Archive:', archive, flush=True)
        print('Completed' if not errors else 'Completed with errors; inspect status.json', flush=True)
    return int(bool(errors))

if __name__ == '__main__':
    raise SystemExit(main())
