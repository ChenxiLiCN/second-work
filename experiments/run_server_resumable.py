#!/usr/bin/env python3
"""One-run mode 11 vs 13 comparison; reuse retained indices, Linux server only."""
import math
import os
from pathlib import Path
import platform
import tarfile
import tempfile
import time
from run_server_ablation import ROOT, checked, digest, measured
from run_server_core import save_json, write_table, output_hashes
from run_server_hotspots import CASES, find_source

VARIANTS=(('control','bri_query_core_lean','11'),('resumable','bri_query_core_resumable','13'))

def validate(m,mode,hashes,reference):
    if m.get('status')!=0 or hashes!=reference: return False
    flags=dict(block_mode=mode,resumable_neighborhoods=str(int(mode=='13')),
        anchor_filter='0',single_pass='1',lean_workspaces='1',cache_budget_mib='32',
        used_roundtrip_metadata='0',witness_counts_built='0')
    if any(m.get(k)!=v for k,v in flags.items()) or m.get('hotspot_diagnostic')=='1': return False
    try:
        for k in ('on_demand_fli_build_ms','query_call_ms','resume_replay_ms','resume_save_ms'):
            if not math.isfinite(float(m[k])) or float(m[k])<0: return False
        if float(m['query_call_ms'])<=0: return False
        if int(m['adaptive_peak_bytes'])+int(m['adaptive_pair_bound_bytes'])>32*1024*1024: return False
        if any(int(m[k])<0 for k in ('adaptive_peak_bytes','adaptive_pair_bound_bytes',
                                     'adaptive_posting_entries','adaptive_streaming_entries')): return False
        if mode=='13':
            if int(m['resume_checks'])!=int(m['adaptive_streaming_checks']): return False
            if int(m['single_pass_complete'])+int(m['single_pass_partial'])!=int(m['resume_checks']): return False
            if int(m['resume_partial_saves'])+int(m['resume_partial_drops'])!=int(m['single_pass_partial']): return False
            if int(m['resume_new_entries'])>int(m['adaptive_posting_entries'])+int(m['adaptive_streaming_entries']): return False
        elif int(m['resume_checks'])!=0: return False
        return True
    except (KeyError,TypeError,ValueError): return False

def main():
    results=Path(os.environ.get('RESULT_ROOT',ROOT/'server-results')).resolve()
    build=Path(os.environ.get('BUILD_DIR',ROOT/'build-linux-resumable')).resolve()
    jobs=int(os.environ.get('BUILD_JOBS','4'));limit=int(os.environ.get('LIMIT_SECONDS','7200'))
    if min(jobs,limit)<1: raise ValueError('Positive BUILD_JOBS/LIMIT_SECONDS required')
    results.mkdir(parents=True,exist_ok=True)
    run=Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-resumable-'),dir=results))
    print('Results:',run,flush=True)
    rows=[];errors=[]
    try:
        source_run,source=find_source(results)
        print('Reusing verified indices:',source_run,flush=True)
        env=dict(version='resumable_v1',platform=platform.platform(),source_run=str(source_run),
            cases=CASES,repeats=1,cache_mib=32,variants=VARIANTS,
            timing='online_compute_s=(on_demand_fli_build_ms+query_call_ms)/1000; excludes index reading and output writing. Replay/save times are nested in query time, not additive.',
            scope='Query-local partial neighborhoods/cursors only. No raw conversion, offline rebuild, anchor filter, persistent projected graphs, or new clustering parameters.',
            caveats='Exploratory one-run comparison; alternate variant order across datasets. No cold-cache claim. No HINSCAN/offline rerun this round; use source run for historical baseline/build times. Skipped-prefix entries are a reuse counter, not measured speedup.')
        env['source_sha256']={str(p.relative_to(ROOT)):digest(p) for base in (ROOT/'src',ROOT/'experiments')
            for p in base.rglob('*') if p.is_file() and p.suffix in ('.cpp','.h','.py')}
        env['source_sha256']['CMakeLists.txt']=digest(ROOT/'CMakeLists.txt')
        env['reused_indices']={name:dict(path=str(s['index']),sha256=s['sha'],index_bytes=s['index'].stat().st_size)
                              for name,s in source.items()}
        save_json(run/'environment.json',env)
        for name,s in source.items():
            if digest(s['index'])!=s['sha']: raise RuntimeError(f'{name}: input index hash mismatch')
        checked(['cmake','-S',ROOT,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_COMPILER=g++'],run/'configure.log')
        checked(['cmake','--build',build,'--parallel',jobs,'--target',
                 'bri_query_core_lean','bri_query_core_resumable','verify_resumable_cache','verify_anchor_filter'],run/'build.log')
        checked(['python3','-B','-m','unittest','discover','-s',ROOT/'experiments','-p','test_server_resumable.py'],run/'script-tests.log')
        for target in ('verify_resumable_cache','verify_anchor_filter'):
            with tempfile.TemporaryDirectory(prefix='oracle-',dir=run) as tmp:
                oracle=measured(run/target,[build/'bin'/target,tmp],limit)
            if oracle['status']!=0 or oracle.get('all_passed')!='1': raise RuntimeError(f'{target}: oracle failed')
        for i,(name,(path,eps)) in enumerate(CASES.items()):
            case=run/name;case.mkdir();s=source[name];pair=[]
            for variant,exe,mode in (VARIANTS if i%2==0 else VARIANTS[::-1]):
                print(f'[{name}] {variant} (once)',flush=True)
                try:
                    with tempfile.TemporaryDirectory(prefix='output-',dir=case) as tmp:
                        metric=measured(case/variant,[build/'bin'/exe,s['index'],path,eps,'5',tmp],limit)
                        hashes=output_hashes(Path(tmp),eps) if metric['status']==0 else None
                    save_json(case/(variant+'-hashes.json'),hashes)
                    save_json(case/(variant+'-metrics.json'),metric)
                    valid=validate(metric,mode,hashes,s['hashes']) and digest(s['index'])==s['sha']
                    row=dict(dataset=name,meta_path=path,epsilon=eps,mu=5,variant=variant,
                             valid=int(valid),index_bytes=s['index'].stat().st_size,**metric)
                    if valid:
                        row['online_compute_s']=(float(metric['on_demand_fli_build_ms'])+float(metric['query_call_ms']))/1000
                        row['neighborhood_scan_entries']=int(metric['adaptive_posting_entries'])+int(metric['adaptive_streaming_entries'])
                    else: errors.append(f'{name}/{variant}: output/configuration/counter/index validation failed')
                    rows.append(row);pair.append(row)
                except Exception as exc: errors.append(f'{name}/{variant}: {exc}')
                finally: write_table(run/'summary.tsv',rows)
            if len(pair)==2 and all(r['valid'] for r in pair):
                by={r['variant']:r for r in pair};a=by['control'];b=by['resumable']
                b['speedup_vs_control']=a['online_compute_s']/b['online_compute_s']
                b['online_saved_s']=a['online_compute_s']-b['online_compute_s']
                b['scan_entries_saved']=a['neighborhood_scan_entries']-b['neighborhood_scan_entries']
                print(f"[{name}] online: control={a['online_compute_s']:.6f}s, resumable={b['online_compute_s']:.6f}s, ratio={b['speedup_vs_control']:.3f}x",flush=True)
    except Exception as exc:
        errors.append(str(exc));print('ERROR:',exc,flush=True)
    finally:
        if len(rows)!=2*len(CASES): errors.append(f'Incomplete comparison: {len(rows)}/{2*len(CASES)}')
        write_table(run/'summary.tsv',rows)
        save_json(run/'status.json',dict(all_passed=not errors,errors=errors,query_runs=len(rows)))
        archive=Path(str(run)+'.tar.gz')
        with tarfile.open(archive,'w:gz') as tar:
            for p in sorted(run.rglob('*')):
                if p.is_file() and p.suffix in ('.log','.time','.json','.tsv'):
                    tar.add(p,arcname=str(Path(run.name)/p.relative_to(run)))
        print('Archive:',archive,flush=True)
        print('Completed' if not errors else 'Completed with errors; inspect status.json',flush=True)
    return int(bool(errors))

if __name__=='__main__':
    if os.name!='posix': raise SystemExit('Run this comparison on the Linux server, not local Windows.')
    raise SystemExit(main())
