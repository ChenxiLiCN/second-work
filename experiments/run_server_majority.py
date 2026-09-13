#!/usr/bin/env python3
"""Fresh, one-run majority-index experiment on the three approved datasets."""
import math
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tarfile
import tempfile
import time

from prepare_selected_data import SPECS, prepare
from run_server_ablation import ROOT, checked, digest, fields
from run_server_core import save_json, write_table

MU = 5


def measured(prefix, command, limit):
    # Independent wrapper: historical runners reject current role/mu semantics.
    started = time.perf_counter()
    with prefix.with_suffix('.log').open('w', encoding='utf-8') as output:
        run = subprocess.run(['/usr/bin/time', '-f', 'max_rss_kb=%M', '-o',
                              str(prefix.with_suffix('.time')), 'timeout', '--kill-after=30s',
                              str(limit)+'s', *map(str, command)], stdout=output,
                             stderr=subprocess.STDOUT)
    metrics = fields(prefix.with_suffix('.log'))
    if prefix.with_suffix('.time').exists():
        metrics.update(fields(prefix.with_suffix('.time')))
    metrics.update(status=run.returncode, elapsed_s=time.perf_counter()-started)
    return metrics


def seconds(metrics, key):
    if metrics.get('status') != 0:
        raise RuntimeError('command failed: '+str(metrics.get('status')))
    value = float(metrics[key])/1000
    if not math.isfinite(value) or value < 0:
        raise RuntimeError('invalid timer: '+key)
    return value


def hashes(directory, epsilon, mu=MU):
    return {kind: digest(directory/f'{kind}-{epsilon}-{mu}.txt') for kind in ('result','roles')}


def benchmark_query(bin_dir, raw, index, directory, path, epsilon, limit):
    """One baseline, one unchanged control and one new query; no repeats."""
    directory.mkdir()
    baseline_hash = None
    with tempfile.TemporaryDirectory(prefix='projection-', dir=directory) as tmp:
        projected = Path(tmp)
        mat = measured(directory/'materialize', [bin_dir/'hin_materialize', raw, path, projected], limit)
        materialize_s = seconds(mat, 'materialize_ms')
        scan = measured(directory/'pscan', [bin_dir/'pscan_baseline', projected, epsilon, str(MU-1), 'output'], limit)
        match = re.search(r'Total time without IO:\s*(\d+)', (directory/'pscan.log').read_text())
        if scan['status'] != 0 or not match:
            raise RuntimeError('upstream pSCAN failed or compute timer missing')
        baseline_s = materialize_s+int(match[1])/1_000_000
        baseline_hash = digest(projected/f'result-{epsilon}-{MU-1}.txt')
        save_json(directory/'baseline.json', dict(materialize=mat, pscan=scan,
                  compute_s=baseline_s, result_sha256=baseline_hash, paper_mu=MU, pscan_other_mu=MU-1))
    outputs, metrics = {}, {}
    for label, exe, source in [('control','bri_query_core_connectivity',index/'base.bri'),
                                ('majority','mgi_query_index',index)]:
        with tempfile.TemporaryDirectory(prefix=label+'-',dir=directory) as tmp:
            m = measured(directory/label,[bin_dir/exe,source,path,epsilon,str(MU),tmp],limit)
            seconds(m,'online_compute_ms')
            if m.get('semantics_version') != 'hinscan_nonindependent_v1' or m.get('mu_counts_self') != '1':
                raise RuntimeError('wrong executable semantics: '+label)
            if m.get('pscan_other_mu') != str(MU-1):
                raise RuntimeError('wrong mu conversion: '+label)
            if label == 'majority':
                if m.get('algorithm') != 'majority_layered_v1' or m.get('fallback_block_mode') != '9':
                    raise RuntimeError('wrong majority executable')
            elif m.get('block_mode') != '9':
                raise RuntimeError('wrong PSCAN control')
            outputs[label] = hashes(Path(tmp),epsilon)
            metrics[label] = m
            save_json(directory/(label+'-hashes.json'),outputs[label])
    if outputs['control'] != outputs['majority'] or outputs['majority']['result'] != baseline_hash:
        raise RuntimeError('cluster or role mismatch; no speedup reported')
    online_s = seconds(metrics['majority'],'online_compute_ms')
    result = dict(meta_path=path,epsilon=epsilon,mu=MU,valid=1,
                  hinscan_compute_s=baseline_s,online_compute_s=online_s,
                  control_online_compute_s=seconds(metrics['control'],'online_compute_ms'),
                  speedup_vs_hinscan=baseline_s/online_s if online_s > 0 else '')
    result.update({k:v for k,v in metrics['majority'].items() if k not in ('mu',)})
    return result


def main():
    if platform.system() != 'Linux':
        raise RuntimeError('Run this experiment on the Linux server, not the local Windows machine')
    data = Path(os.environ.get('DATA_ROOT',ROOT/'data')).resolve()
    build = Path(os.environ.get('BUILD_DIR',ROOT/'build-linux-majority')).resolve()
    root = Path(os.environ.get('RESULT_ROOT',ROOT/'server-results')).resolve()
    jobs,limit = int(os.environ.get('BUILD_JOBS','2')),int(os.environ.get('LIMIT_SECONDS','7200'))
    if min(jobs,limit)<1: raise ValueError('positive BUILD_JOBS and LIMIT_SECONDS required')
    root.mkdir(parents=True,exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-majority-'),dir=root))
    print('Results:',run,flush=True)
    rows,offline,errors = [],[],[]
    bin_dir = build/'bin'
    try:
        for name,(source,_,_) in SPECS.items():
            if not (data/source/'base.txt').is_file():
                raise RuntimeError(f'Missing dataset: {data/source}')
        env = dict(version='majority_v1',repeats=1,platform=platform.platform(),mu=MU,
                   cases={name:spec[2] for name,spec in SPECS.items()},
                   timing='Offline excludes raw input and index writes. Online excludes index reads/validation and output writes, includes planner/factors/PSCAN/merge. HINSCAN reconstruction = projection compute + upstream PSCAN compute without IO.',
                   limitation='Not official HINSCAN source. Upstream baseline supplies clusters, not complete HINSCAN roles. Roles checked against current mode 9 plus independent tiny oracle; do not claim identical output-work timing scope.',
                   index='Graph/schema only: existing BRI v2 plus original-relation majority groups; no path graph/similarity table. No query learned index.',
                   provenance='Existing lossless data adapters; no sampling or historical timing reuse.')
        env['source_sha256']={str(p.relative_to(ROOT)):digest(p) for folder in (ROOT/'src',ROOT/'experiments')
                              for p in folder.rglob('*') if p.is_file() and p.suffix in ('.cpp','.h','.py')}
        env['source_sha256']['CMakeLists.txt']=digest(ROOT/'CMakeLists.txt')
        save_json(run/'environment.json',env)
        checked(['cmake','-S',ROOT,'-B',build,'-DCMAKE_BUILD_TYPE=Release',
                 '-DCMAKE_CXX_COMPILER=g++','-DBUILD_PSCAN_BASELINE=ON'],run/'configure.log')
        checked(['cmake','--build',build,'--parallel',jobs,'--target','mgi_build_index',
                 'mgi_query_index','verify_majority_index','verify_selected_factor',
                 'bri_query_core_connectivity','hin_materialize','pscan_baseline'],run/'build.log')
        checked([sys.executable,'-B','-m','unittest','discover','-s',ROOT/'experiments',
                 '-p','test_server_majority.py'],run/'runner-tests.log')
        with tempfile.TemporaryDirectory(prefix='verify-',dir=run) as tmp:
            checked([bin_dir/'verify_majority_index',Path(tmp)/'index'],run/'verify-index.log')
            checked([bin_dir/'verify_selected_factor',Path(tmp)/'factor'],run/'verify-factor.log')
        print('Checking independent tiny C++ oracle',flush=True)
        checked([sys.executable,'-B',ROOT/'experiments/audit_majority_cpp.py','--bin-dir',bin_dir],run/'oracle.log')
        for name in SPECS:
            case=run/name;case.mkdir()
            try:
                print(f'[{name}] lossless normalization and offline build (once)',flush=True)
                # Only generated normalized data is temporary; original data is untouched.
                with tempfile.TemporaryDirectory(prefix='normalized-',dir=case) as tmp:
                    raw=Path(tmp)/'hin'
                    report=prepare(name,data,raw);save_json(case/'data-audit.json',report)
                    raw_bytes=sum(p.stat().st_size for p in raw.rglob('*.txt') if '-id-map' not in p.name)
                    index=case/'index'
                    off=measured(case/'offline',[bin_dir/'mgi_build_index',raw,index],limit)
                    offline_s=seconds(off,'offline_compute_ms')
                    size=sum((index/f).stat().st_size for f in ('base.bri','groups.mgi'))
                    if int(off['index_bytes'])!=size: raise RuntimeError('index byte count mismatch')
                    before={f:digest(index/f) for f in ('base.bri','groups.mgi')}
                    offline.append(dict(dataset=name,offline_compute_s=offline_s,normalized_raw_bytes=raw_bytes,
                                        index_to_normalized_text_ratio=size/raw_bytes if raw_bytes else '',
                                        **off))
                    save_json(case/'index-hashes.json',before)
                    for path,epsilon in SPECS[name][2]:
                        print(f'[{name}/{path}] fresh baseline, control, majority (each once)',flush=True)
                        row=benchmark_query(bin_dir,raw,index,case/path,path,epsilon,limit)
                        row.update(dataset=name,offline_compute_s=offline_s,index_bytes=size)
                        rows.append(row)
                    if before!={f:digest(index/f) for f in before}:
                        raise RuntimeError('index changed during queries')
            except Exception as exc:
                errors.append(f'{name}: {exc}');print('ERROR:',errors[-1],flush=True)
                for row in rows:
                    if row['dataset']==name:
                        row['valid']=0;row.pop('speedup_vs_hinscan',None)
            finally:
                write_table(run/'runs.tsv',rows);write_table(run/'offline.tsv',offline)
    except Exception as exc:
        errors.append(str(exc));print('ERROR:',exc,flush=True)
    finally:
        expected=sum(len(s[2]) for s in SPECS.values())
        if len(rows)!=expected or len(offline)!=len(SPECS):
            errors.append(f'Incomplete: {len(rows)}/{expected} queries, {len(offline)}/{len(SPECS)} builds')
        write_table(run/'runs.tsv',rows);write_table(run/'offline.tsv',offline)
        keys=('dataset','meta_path','epsilon','mu','valid','hinscan_compute_s','offline_compute_s',
              'online_compute_s','speedup_vs_hinscan','index_bytes','control_online_compute_s',
              'completed_core_vertices','completed_noncore_vertices','residual_vertices','layer_proof_ms',
              'residual_prepare_ms','residual_scan_ms')
        write_table(run/'summary.tsv',[{k:r.get(k,'') for k in keys} for r in rows])
        save_json(run/'status.json',dict(all_passed=not errors,errors=errors,query_runs=len(rows),offline_runs=len(offline)))
        archive=Path(str(run)+'.tar.gz')
        with tarfile.open(archive,'w:gz') as tar:
            for p in sorted(run.rglob('*')):
                if p.is_file() and p.suffix in ('.json','.tsv','.log','.time'):
                    tar.add(p,arcname=str(Path(run.name)/p.relative_to(run)))
        print('Send this archive:',archive,flush=True)
    return int(bool(errors))


if __name__=='__main__':
    raise SystemExit(main())
