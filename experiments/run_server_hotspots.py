#!/usr/bin/env python3
"""Read-only index reuse; one diagnostic query each for IMDB large and Foursquare."""
import csv
import json
import os
from pathlib import Path
import platform
import shutil
import tarfile
import tempfile
import time
from run_server_ablation import ROOT, checked, digest, fields, measured
from run_server_core import save_json, write_table, output_hashes

CASES={'imdb_large':('movie-actor-movie','0.5'),
       'foursquare':('user-venue-user','0.5')}

def find_source(results):
    for run in sorted((p for p in results.glob('*-anchor-*') if p.is_dir()),reverse=True):
        try:
            status=json.loads((run/'status.json').read_text())
            env=json.loads((run/'environment.json').read_text())
            if status.get('all_passed') is not True or env.get('version')!='anchor_v1': continue
            with (run/'offline.tsv').open(newline='') as f:
                offline={r['dataset']:r for r in csv.DictReader(f,delimiter='\t')}
            source={}
            for name,(path,eps) in CASES.items():
                q=run/name/path;index=run/name/'base.bri'
                reference=json.loads((q/'control-hashes.json').read_text())
                spec=env['cases'][name]
                config=fields(q/'control.log')
                if spec['mu']!=5 or [path,eps] not in spec['queries']: raise ValueError('query mismatch')
                if config.get('block_mode')!='11' or config.get('used_roundtrip_metadata')!='0': raise ValueError('reference mode mismatch')
                if not all(reference.get(k) for k in ('result','roles')): raise ValueError('missing reference hashes')
                if not index.is_file() or index.stat().st_size!=int(offline[name]['index_bytes']): raise ValueError('missing index')
                source[name]=dict(index=index,sha=offline[name]['sha256'],hashes=reference)
            return run,source
        except (OSError,ValueError,KeyError,TypeError):
            continue
    raise RuntimeError('No complete anchor_v1 run with retained base.bri files found in server-results. '
                       'Keep the unpacked server experiment directory; the small tar.gz alone contains no indices.')

def validate(metric,report,hashes,reference):
    if metric['status']!=0 or hashes!=reference: return False
    if any(metric.get(k)!=v for k,v in dict(hotspot_diagnostic='1',block_mode='11',
            anchor_filter='0',single_pass='1',lean_workspaces='1',cache_budget_mib='32',
            used_roundtrip_metadata='0',witness_counts_built='0').items()): return False
    try:
        return (report.get('diagnostic_only')=='1'
            and int(report['stream_entries'])==int(metric['adaptive_streaming_entries'])
            and int(report['generation_entries'])==int(metric['adaptive_posting_entries'])
            and int(report['stream_calls'])==int(metric['adaptive_streaming_checks'])
            and int(report['partial_calls'])==int(metric['single_pass_partial'])
            and int(report['scan_entries'])==int(report['stream_entries'])+int(report['generation_entries']))
    except (ValueError,KeyError): return False

def main():
    results=Path(os.environ.get('RESULT_ROOT',ROOT/'server-results')).resolve()
    build=Path(os.environ.get('BUILD_DIR',ROOT/'build-linux-hotspots')).resolve()
    jobs=int(os.environ.get('BUILD_JOBS','4')); limit=int(os.environ.get('LIMIT_SECONDS','7200'))
    if min(jobs,limit)<1: raise ValueError('Positive BUILD_JOBS/LIMIT_SECONDS required')
    results.mkdir(parents=True,exist_ok=True)
    run=Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-hotspots-'),dir=results))
    print('Results:',run,flush=True)
    rows=[];errors=[]
    try:
        source_run,source=find_source(results)
        print('Reusing indices and reference hashes:',source_run,flush=True)
        env=dict(version='hotspots_v1',platform=platform.platform(),source_run=str(source_run),
            cases=CASES,repeats=1,
            scope='Diagnostic mode 11 only. No raw conversion, index build, HINSCAN rerun or algorithm change.',
            caveats='Instrumented query and post-query coverage are NOT performance benchmarks. Ranking is by raw entries, not elapsed time. Canonical rows share identical factors. Coverage uses largest 1/2/4 postings, not optimal subsets.',
            report_limits='512 hottest rows; at most 50 million posting entries for exact coverage. Incomplete rows are marked. Weighted coverage applies only to measured rows; coverage_work_share reports their fraction of all work.')
        env['source_sha256']={str(p.relative_to(ROOT)):digest(p) for base in (ROOT/'src',ROOT/'experiments')
            for p in base.rglob('*') if p.is_file() and p.suffix in ('.cpp','.h','.py')}
        env['source_sha256']['CMakeLists.txt']=digest(ROOT/'CMakeLists.txt')
        env['reused_indices']={name:dict(path=str(s['index']),sha256=s['sha']) for name,s in source.items()}
        save_json(run/'environment.json',env)
        for name,s in source.items():
            if digest(s['index'])!=s['sha']: raise RuntimeError(f'{name}: input index hash mismatch')
        checked(['cmake','-S',ROOT,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_COMPILER=g++'],run/'configure.log')
        checked(['cmake','--build',build,'--parallel',jobs,'--target','bri_diagnose_hotspots','verify_hotspot_diagnostics'],run/'build.log')
        checked(['python3','-B','-m','unittest','discover','-s',ROOT/'experiments','-p','test_server_hotspots.py'],run/'script-tests.log')
        with tempfile.TemporaryDirectory(prefix='oracle-',dir=run) as tmp:
            oracle=measured(run/'oracle',[build/'bin/verify_hotspot_diagnostics',tmp],limit)
        if oracle['status']!=0 or oracle.get('all_passed')!='1': raise RuntimeError('Diagnostic oracle failed')
        for name,(path,eps) in CASES.items():
            case=run/name;case.mkdir();s=source[name]
            try:
                print(f'[{name}] diagnostic mode 11 (once); coverage report follows query',flush=True)
                with tempfile.TemporaryDirectory(prefix='output-',dir=case) as tmp:
                    out=Path(tmp)
                    metric=measured(case/'diagnostic',[build/'bin/bri_diagnose_hotspots',s['index'],path,eps,'5',out],limit)
                    hashes=output_hashes(out,eps) if metric['status']==0 else None
                    report=fields(out/'hotspot-summary.log') if (out/'hotspot-summary.log').is_file() else {}
                    reports_present=all((out/p).is_file() for p in ('hotspot-summary.log','hotspots.tsv'))
                    for filename in ('hotspot-summary.log','hotspots.tsv'):
                        if (out/filename).is_file(): shutil.copy2(out/filename,case/filename)
                save_json(case/'output-hashes.json',hashes)
                valid=reports_present and validate(metric,report,hashes,s['hashes']) and digest(s['index'])==s['sha']
                row=dict(dataset=name,meta_path=path,epsilon=eps,mu=5,valid=int(valid),**report)
                row['diagnostic_elapsed_s']=metric.get('elapsed_s','')
                row['diagnostic_peak_rss_kb']=metric.get('max_rss_kb','')
                rows.append(row)
                if not valid: errors.append(f'{name}: result/configuration/counter/index validation failed')
            except Exception as exc: errors.append(f'{name}: {exc}')
            finally: write_table(run/'summary.tsv',rows)
    except Exception as exc:
        errors.append(str(exc));print('ERROR:',exc,flush=True)
    finally:
        if len(rows)!=len(CASES): errors.append(f'Incomplete diagnosis: {len(rows)}/{len(CASES)}')
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
    if os.name!='posix': raise SystemExit('Run this diagnostic on the Linux server, not local Windows.')
    raise SystemExit(main())
