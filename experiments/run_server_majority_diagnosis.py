#!/usr/bin/env python3
"""Reuse verified majority indices; compare current mode 9 work, once per query."""
import json
import math
import os
from pathlib import Path
import platform
import sys
import tarfile
import tempfile
import time

from run_server_majority import ROOT, SPECS, checked, digest, hashes, measured, save_json, write_table

CASES = {name: spec[2][0] for name, spec in SPECS.items()}
COUNTERS = ('candidate_vertices_emitted', 'candidate_posting_entries_read',
            'exact_similarity_checks', 'certificate_entries', 'projected_edges',
            'certified_blocks', 'block_core_vertices', 'adaptive_posting_entries',
            'adaptive_streaming_entries', 'adaptive_streaming_checks',
            'adaptive_intersection_units', 'adaptive_hits', 'adaptive_misses',
            'adaptive_evictions', 'adaptive_distinct_factor_rows',
            'witness_bound_checks', 'witness_bound_accepts', 'witness_bound_rejects',
            'witness_entries_read', 'witness_counts_built', 'activation_calls',
            'activation_full_words_written')
TIMERS = ('prune_ms', 'core_ms', 'noncore_ms', 'role_ms', 'block_discovery_ms',
          'adaptive_generation_ms', 'adaptive_streaming_ms', 'half_expansion_ms',
          'degree_compute_ms', 'posting_order_ms')


def load_source(run):
    def read(p): return json.loads(p.read_text(encoding='utf-8'))
    status, env = read(run/'status.json'), read(run/'environment.json')
    if (status.get('all_passed') is not True or status.get('query_runs') != 3
            or status.get('offline_runs') != 3 or env.get('version') != 'majority_v1'
            or env.get('mu') != 5):
        raise ValueError('Source must be a complete majority_v1 run with mu=5')
    sources = {}
    for name,(path,eps) in CASES.items():
        if env['cases'][name] != [[path,eps]]: raise ValueError('Source query mismatch: '+name)
        case = run/name
        index = case/'index'
        expected = read(case/'index-hashes.json')
        if set(expected) != {'base.bri','groups.mgi'}: raise ValueError('Missing index hashes')
        for filename,sha in expected.items():
            if digest(index/filename) != sha: raise ValueError('Index hash mismatch: '+str(index/filename))
        reference = read(case/path/'majority-hashes.json')
        if (set(reference) != {'result','roles'} or
                any(not isinstance(v,str) or len(v)!=64 for v in reference.values()) or
                reference != read(case/path/'control-hashes.json') or
                reference['result'] != read(case/path/'baseline.json')['result_sha256']):
            raise ValueError('Source output references disagree: '+name)
        sources[name] = dict(index=index, index_hashes=expected, reference=reference)
    return sources


def find_source(root):
    explicit = os.environ.get('SOURCE_RUN')
    if explicit:
        run = Path(explicit).resolve()
        return run, load_source(run)
    rejected = []
    for run in sorted(root.glob('*-majority-*'), reverse=True):
        if not run.is_dir() or '-majority-diagnosis-' in run.name: continue
        try: return run, load_source(run)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            rejected.append(f'{run.name}: {exc}')
    raise RuntimeError('No verified majority_v1 run with retained index/base.bri and '
                       'index/groups.mgi. The small tar.gz has no indices. '
                       'Set SOURCE_RUN to the original server run directory. '+ '; '.join(rejected))


def compare(control, majority):
    for m in (control,majority):
        if (m.get('status') != 0 or m.get('semantics_version') != 'hinscan_nonindependent_v1'
                or m.get('mu_counts_self') != '1' or m.get('pscan_other_mu') != '4'):
            raise ValueError('Query failed or wrong semantics')
    if (control.get('block_mode') != '9' or majority.get('fallback_block_mode') != '9'
            or majority.get('algorithm') != 'majority_layered_v1'
            or majority.get('residual_diagnostics_version') != '1'):
        raise ValueError('Wrong mode or missing residual diagnostics')
    rows = []
    for key in COUNTERS + TIMERS + ('online_compute_ms',):
        other = key if key == 'online_compute_ms' else 'residual_'+key
        try:
            a,b = (int(control[key]), int(majority[other])) if key in COUNTERS else (
                float(control[key]), float(majority[other]))
        except (KeyError, TypeError, ValueError) as exc:
            raise ValueError('Missing or invalid diagnostic: '+key) from exc
        if not all(math.isfinite(v) and v >= 0 for v in (a,b)):
            raise ValueError('Invalid diagnostic: '+key)
        rows.append(dict(metric=key, control=a, majority=b, majority_to_control=b/a if a else '',
                         kind='counter' if key in COUNTERS else 'time_ms'))
    return rows


def diagnose(bin_dir, source, case, path, eps, limit):
    metrics = {}
    for label,exe in [('control','bri_query_core_connectivity'), ('majority','mgi_query_index')]:
        input_path = source['index']/'base.bri' if label == 'control' else source['index']
        with tempfile.TemporaryDirectory(prefix=label+'-',dir=case) as tmp:
            metric = measured(case/label, [bin_dir/exe,input_path,path,eps,'5',tmp],limit)
            if metric['status'] != 0: raise RuntimeError(label+' query failed')
            actual = hashes(Path(tmp),eps)
            save_json(case/(label+'-hashes.json'),actual)
            if actual != source['reference']: raise RuntimeError(label+' cluster/role mismatch')
            metrics[label] = metric
    for filename,sha in source['index_hashes'].items():
        if digest(source['index']/filename) != sha: raise RuntimeError('Input index changed')
    rows = compare(metrics['control'],metrics['majority'])
    save_json(case/'metrics.json',metrics)
    return rows


def main():
    if platform.system() != 'Linux': raise RuntimeError('Run on the Linux server; no local real-data runs')
    root = Path(os.environ.get('RESULT_ROOT',ROOT/'server-results')).resolve()
    build = Path(os.environ.get('BUILD_DIR',ROOT/'build-linux-majority-diagnosis')).resolve()
    jobs,limit = int(os.environ.get('BUILD_JOBS','2')),int(os.environ.get('LIMIT_SECONDS','7200'))
    if min(jobs,limit)<1: raise ValueError('Positive jobs and limit required')
    root.mkdir(parents=True,exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-majority-diagnosis-'),dir=root))
    print('Results:',run,flush=True)
    rows,errors = [],[]
    completed = 0
    try:
        original,sources = find_source(root)
        print('Reusing verified indices:',original,flush=True)
        provenance = {str(p.relative_to(ROOT)):digest(p) for folder in (ROOT/'src',ROOT/'experiments')
                      for p in folder.rglob('*') if p.is_file() and p.suffix in ('.cpp','.h','.py')}
        provenance['CMakeLists.txt'] = digest(ROOT/'CMakeLists.txt')
        save_json(run/'environment.json',dict(version='majority_diagnosis_v1',source_run=str(original),
                  repeats=1,cases=CASES,mu=5,platform=platform.platform(),source_sha256=provenance,
                  reused_indices={n:dict(path=str(s['index']),hashes=s['index_hashes'],
                                          reference=s['reference']) for n,s in sources.items()},
                  scope='Fresh control then majority, mode 9. No offline build or real-data baseline rerun.',
                  timing='Single observations, not stable speedups. Online excludes input/output. '
                         'Subtimers overlap; do not sum all rows. Ratios are majority/control.',
                  counters='Residual scan work only; excludes majority proof work. See majority.log for proof counters.'))
        checked(['cmake','-S',ROOT,'-B',build,'-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_CXX_COMPILER=g++','-DBUILD_PSCAN_BASELINE=ON'],run/'configure.log')
        checked(['cmake','--build',build,'--parallel',jobs,'--target','mgi_build_index',
                 'mgi_query_index','bri_query_core_connectivity','hin_materialize','pscan_baseline'],run/'build.log')
        checked([sys.executable,'-B','-m','unittest','discover','-s',ROOT/'experiments',
                 '-p','test_server_majority*.py'],run/'runner-tests.log')
        checked([sys.executable,'-B',ROOT/'experiments/audit_majority_cpp.py',
                 '--bin-dir',build/'bin'],run/'oracle.log')
        for name,(path,eps) in CASES.items():
            case = run/name
            case.mkdir()
            try:
                print(f'[{name}] control and majority once',flush=True)
                comparison = diagnose(build/'bin',sources[name],case,path,eps,limit)
                rows.extend(dict(dataset=name,meta_path=path,epsilon=eps,mu=5,valid=1,**r) for r in comparison)
                completed += 1
            except Exception as exc:
                errors.append(f'{name}: {exc}')
            write_table(run/'comparison.tsv',rows)
    except Exception as exc:
        errors.append(str(exc))
    finally:
        if completed != len(CASES): errors.append(f'Incomplete: {completed}/{len(CASES)} pairs')
        write_table(run/'comparison.tsv',rows)
        save_json(run/'status.json',dict(all_passed=not errors,errors=errors,query_pairs=completed))
        archive = Path(str(run)+'.tar.gz')
        with tarfile.open(archive,'w:gz') as out:
            for p in sorted(run.rglob('*')):
                if p.is_file() and p.suffix in ('.json','.tsv','.log','.time'):
                    out.add(p,arcname=str(Path(run.name)/p.relative_to(run)))
        for error in errors: print('ERROR:',error,flush=True)
        print('Send this archive:',archive,flush=True)
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
