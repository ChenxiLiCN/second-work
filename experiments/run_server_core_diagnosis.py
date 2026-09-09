#!/usr/bin/env python3
import csv
import json
import os
from pathlib import Path
import tarfile
import tempfile
import time
from run_server_ablation import ROOT, checked, digest, measured
from run_server_core import output_hashes, save_json, write_table

DATASETS = ('dblp_apv_full', 'dblp_v18')

def select_source(root):
    for p in sorted(root.glob('*-core-*'), reverse=True):
        if p.is_dir() and (p / 'offline.tsv').is_file() and all(
            (p / n / 'enhanced.bri').is_file() and
            (p / n / 'combined-hashes.json').is_file() for n in DATASETS):
            return p
    raise RuntimeError('Previous core result directory with both enhanced.bri files required. Log archives do not contain indexes. No rebuild attempted.')

def main():
    results = Path(os.environ.get('RESULT_ROOT', ROOT / 'server-results')).resolve()
    build = Path(os.environ.get('BUILD_DIR', ROOT / 'build-linux-core')).resolve()
    search = Path(os.environ.get('INDEX_SEARCH_ROOT', results)).resolve()
    jobs, limit = int(os.environ.get('BUILD_JOBS', '4')), int(os.environ.get('LIMIT_SECONDS', '7200'))
    if min(jobs, limit) < 1:
        raise ValueError('BUILD_JOBS and LIMIT_SECONDS must be positive')
    results.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-core-diagnosis-'), dir=results))
    print('Results:', run, flush=True)
    rows, errors = [], []
    try:
        source = select_source(search)
        print('Reusing indexes:', source, flush=True)
        with (source / 'offline.tsv').open(newline='', encoding='utf-8') as f:
            offline = list(csv.DictReader(f, delimiter='\t'))
        inputs = {}
        for name in DATASETS:
            index = source / name / 'enhanced.bri'
            record = [r for r in offline if r['dataset'] == name and r['index'] == 'enhanced']
            sha = digest(index)
            if len(record) != 1 or record[0]['status'] != '0' or sha != record[0]['sha256']:
                raise RuntimeError(f'Index build record/hash mismatch: {name}')
            inputs[name] = dict(path=str(index), sha256=sha, bytes=index.stat().st_size,
                expected=json.loads((source / name / 'combined-hashes.json').read_text()))
        env = dict(source=str(source), inputs=inputs, cache_mib=32, block_mode=9,
            schedule='Per dataset: uninstrumented p0 once, diagnostic p1 once, diagnostic p2 once.',
            timing='p0 online_compute_s = (on_demand_fli_build_ms + query_call_ms)/1000. Excludes reading index, writing results, exit cleanup.',
            warning='p1/p2 are diagnostic only. Nested inclusive times overlap: do not sum. Independently sampled estimates cannot be subtracted as exact exclusive costs. Retain samples and sampling SE; instrumentation perturbs execution.',
            p1='candidates, activation, adaptive check, witness bounds/count, full intersection',
            p2='state/exact wrappers, certificate get/put/rehash, union-find')
        env['source_sha256'] = {str(p.relative_to(ROOT)): digest(p) for p in (ROOT / 'src').rglob('*')
                               if p.is_file() and p.suffix in ('.cpp', '.h')}
        env['source_sha256']['CMakeLists.txt'] = digest(ROOT / 'CMakeLists.txt')
        save_json(run / 'environment.json', env)
        checked(['cmake', '-S', ROOT, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_CXX_COMPILER=g++'], run / 'configure.log')
        checked(['cmake', '--build', build, '--parallel', jobs, '--target',
                 'bri_query_core_connectivity', 'bri_profile_core_p1', 'bri_profile_core_p2'], run / 'build.log')
        for name in DATASETS:
            case = run / name
            case.mkdir()
            index = Path(inputs[name]['path'])
            for group, exe in [(0, 'bri_query_core_connectivity'), (1, 'bri_profile_core_p1'), (2, 'bri_profile_core_p2')]:
                print(f'[{name}] p{group} (once; p0 timing, p1/p2 diagnostics)', flush=True)
                try:
                    with tempfile.TemporaryDirectory(prefix='output-', dir=case) as tmp:
                        metric = measured(case / f'p{group}', [build / 'bin' / exe, index, 'A-P-A', '0.5', '5', tmp], limit)
                        hashes = output_hashes(Path(tmp), '0.5') if metric['status'] == 0 else None
                    valid = (metric['status'] == 0 and hashes == inputs[name]['expected']
                        and metric.get('block_mode') == '9' and metric.get('cache_budget_mib') == '32'
                        and metric.get('used_roundtrip_metadata') == '1'
                        and metric.get('profile_group', '0') == str(group))
                    row = dict(dataset=name, group=group, valid=int(valid), **metric)
                    if valid and group == 0:
                        row['online_compute_s'] = (float(metric['on_demand_fli_build_ms']) + float(metric['query_call_ms'])) / 1000
                    rows.append(row)
                    save_json(case / f'p{group}-hashes.json', hashes)
                    if not valid:
                        raise RuntimeError('Query failed, output mismatch or wrong executable configuration')
                except Exception as e:
                    errors.append(f'{name}/p{group}: {e}')
                    print(errors[-1], flush=True)
                    break
            if digest(index) != inputs[name]['sha256']:
                for row in rows:
                    if row['dataset'] == name:
                        row['valid'] = 0
                        row.pop('online_compute_s', None)
                raise RuntimeError(f'Index changed during diagnosis: {name}')
    except Exception as e:
        errors.append(str(e))
        print('ERROR:', e, flush=True)
    finally:
        write_table(run / 'timing.tsv', [r for r in rows if r['group'] == 0])
        write_table(run / 'diagnostics.tsv', [r for r in rows if r['group'] != 0])
        save_json(run / 'status.json', dict(all_passed=not errors, errors=errors, runs=len(rows)))
        archive = Path(str(run) + '.tar.gz')
        with tarfile.open(archive, 'w:gz') as tar:
            for p in sorted(run.rglob('*')):
                if p.is_file() and p.suffix in ('.log', '.time', '.json', '.tsv'):
                    tar.add(p, arcname=str(Path(run.name) / p.relative_to(run)))
        print('Archive:', archive, flush=True)
        print('Completed' if not errors else 'Completed with errors; inspect status.json', flush=True)
    return int(bool(errors))

if __name__ == '__main__':
    raise SystemExit(main())
