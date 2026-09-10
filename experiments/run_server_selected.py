#!/usr/bin/env python3
"""Selected Yelp / IMDB-large / Foursquare, one query per dataset, compute-only timings."""
import os
import platform
from pathlib import Path
import re
import tarfile
import tempfile
import time
from run_server_ablation import ROOT, checked, digest, measured
from run_server_core import save_json, write_table, output_hashes
from prepare_selected_data import SPECS, prepare

def main(*, anchor=False):
    data=Path(os.environ.get('DATA_ROOT',ROOT/'data')).resolve()
    build=Path(os.environ.get('BUILD_DIR',ROOT/('build-linux-anchor' if anchor else 'build-linux-selected'))).resolve()
    results=Path(os.environ.get('RESULT_ROOT',ROOT/'server-results')).resolve()
    jobs=int(os.environ.get('BUILD_JOBS','4'))
    limit=int(os.environ.get('LIMIT_SECONDS','7200'))
    if min(jobs,limit)<1: raise ValueError('Positive BUILD_JOBS/LIMIT_SECONDS required')
    results.mkdir(parents=True,exist_ok=True)
    run=Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-')+('anchor-' if anchor else 'selected-'),dir=results))
    print('Results:',run,flush=True)
    rows,offline,errors=[],[],[]
    bin_dir=build/'bin'
    variants=([('control','bri_query_core_lean','11'),('latest','bri_query_core_anchor','12')] if anchor else
              [('control','bri_query_core_single_pass','10'),('latest','bri_query_core_lean','11')])
    try:
        env=dict(version='selected_v1',platform=platform.platform(),repeats=1,
            cases={n:dict(source=v[0],queries=v[2],mu=5) for n,v in SPECS.items()},
            timing='Offline = allocation/population/sort/dedup of typed adjacency from buffered pairs. Online = factor preparation + query call. Both exclude input/output. Baseline = materialize_ms + upstream pSCAN Total time without IO.',
            scope='Basic graph+schema index built ONCE per dataset, without path/epsilon/mu. Latest mode 11 and mode 10 role control. Baseline is local HINSCAN-flow reconstruction, not official HINSCAN source. No claim that this one path represents every path.',
            correctness='Clusters versus upstream pSCAN; roles versus mode 10; independent small-graph oracle.',
            provenance='User selected these datasets; original provider/paper provenance not independently certified.')
        env['source_sha256']={str(p.relative_to(ROOT)):digest(p) for base in (ROOT/'src',ROOT/'experiments')
            for p in base.rglob('*') if p.is_file() and p.suffix in ('.cpp','.h','.py')}
        env['source_sha256']['CMakeLists.txt']=digest(ROOT/'CMakeLists.txt')
        if anchor:
            env.update(version='anchor_v1',
                scope='Same base BRI as mode 11. Mode 12 adds query-local single-anchor rejection and bounded intersection cursors. No batch skipping, no new offline metadata. Anchor selection and filtering are INCLUDED in online time.',
                correctness='Clusters versus upstream pSCAN; roles versus mode 11; independent small-graph anchor oracle and existing oracle.',
                memory='Both variants retain the same 32 MiB neighborhood budget. Mode 12 adds a vertex-to-anchor array and at most 65536 progress records; bytes are reported separately.',
                timing_caveat='Per-call filter timers are included in mode 12 wall compute time. Exact time saved per avoided predicate is NOT measured counterfactually; compare total online times.')
        save_json(run/'environment.json',env)
        for name,(raw,_,_) in SPECS.items():
            if not (data/raw/'base.txt').is_file(): raise RuntimeError(f'Missing raw dataset: {data/raw}')
        checked(['cmake','-S',ROOT,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_COMPILER=g++','-DBUILD_PSCAN_BASELINE=ON'],run/'configure.log')
        targets=['bri_build_measured','hin_materialize','pscan_baseline','verify_adaptive_fli']+[v[1] for v in variants]
        if anchor: targets.append('verify_anchor_filter')
        checked(['cmake','--build',build,'--parallel',jobs,'--target',*targets],run/'build.log')
        checked(['python3','-B','-m','unittest','discover','-s',ROOT/'experiments','-p','test_server_selected.py'],run/'script-tests.log')
        if anchor:
            checked(['python3','-B','-m','unittest','discover','-s',ROOT/'experiments','-p','test_server_anchor.py'],run/'anchor-script-tests.log')
            print('Checking anchor filter oracle',flush=True)
            with tempfile.TemporaryDirectory(prefix='anchor-oracle-',dir=run) as tmp:
                anchor_oracle=measured(run/'anchor-oracle',[bin_dir/'verify_anchor_filter',tmp],limit)
            if anchor_oracle['status']!=0 or anchor_oracle.get('all_passed')!='1': raise RuntimeError('Anchor oracle failed')
        print('Checking independent oracle',flush=True)
        with tempfile.TemporaryDirectory(prefix='oracle-',dir=run) as tmp:
            oracle=measured(run/'oracle',[bin_dir/'verify_adaptive_fli',tmp],limit)
        if oracle['status']!=0 or oracle.get('all_passed')!='1': raise RuntimeError('Oracle failed')
        for name in SPECS:
            case=run/name;case.mkdir()
            try:
                print(f'[{name}] validating and adapting raw format',flush=True)
                normalized=case/'normalized'
                report=prepare(name,data,normalized)
                save_json(case/'data-audit.json',report)
                if report['domain_changes']: print('Domain declaration corrections:',report['domain_changes'],flush=True)
                index=case/'base.bri'
                print(f'[{name}] offline build (once)',flush=True)
                off=measured(case/'offline',[bin_dir/'bri_build_measured',normalized,index],limit)
                if off['status']!=0 or 'offline_compute_ms' not in off: raise RuntimeError('Offline build failed or missing compute timer')
                sha=digest(index)
                off.update(dataset=name,index_bytes=index.stat().st_size,sha256=sha,
                           offline_compute_s=float(off['offline_compute_ms'])/1000)
                offline.append(off)
                for path,eps in SPECS[name][2]:
                    qdir=case/path;qdir.mkdir()
                    print(f'[{name}/{path}] HINSCAN reconstructed full flow (once)',flush=True)
                    baseline_hash=None;baseline_s=None
                    with tempfile.TemporaryDirectory(prefix='projected-',dir=qdir) as tmp:
                        mat=measured(qdir/'materialize',[bin_dir/'hin_materialize',normalized,path,tmp],limit)
                        scan=None
                        if mat['status']==0:
                            scan=measured(qdir/'pscan',[bin_dir/'pscan_baseline',tmp,eps,'5','output'],limit)
                        if scan is not None and scan['status']==0:
                            match=re.search(r'Total time without IO:\s*(\d+)',(qdir/'pscan.log').read_text())
                            if not match or 'materialize_ms' not in mat: raise RuntimeError('Baseline compute timer missing')
                            baseline_s=float(mat['materialize_ms'])/1000+int(match[1])/1000000
                            baseline_hash=digest(Path(tmp)/f'result-{eps}-5.txt')
                        save_json(qdir/'baseline.json',dict(materialize=mat,pscan=scan,compute_s=baseline_s,result_sha256=baseline_hash))
                    if baseline_s is None: errors.append(f'{name}/{path}: baseline failed; no speedup reported')
                    reference=None
                    control_metric=None
                    for variant,exe,mode in variants:
                        print(f'[{name}/{path}] {variant} (once)',flush=True)
                        with tempfile.TemporaryDirectory(prefix='output-',dir=qdir) as tmp:
                            metric=measured(qdir/variant,[bin_dir/exe,index,path,eps,'5',tmp],limit)
                            hashes=output_hashes(Path(tmp),eps) if metric['status']==0 else None
                        config=(metric.get('block_mode')==mode and metric.get('used_roundtrip_metadata')=='0'
                                and metric.get('cache_budget_mib')=='32' and metric.get('single_pass')=='1'
                                and metric.get('lean_workspaces')==str(int(anchor or variant=='latest'))
                                and metric.get('witness_counts_built')=='0')
                        if anchor:
                            config=config and metric.get('anchor_filter')==str(int(variant=='latest'))
                            if variant=='latest':
                                def count(key): return int(metric.get(key,'-1'))
                                required=('anchor_calls','anchor_no_anchor','anchor_ineligible','anchor_eligible',
                                          'anchor_rejects','anchor_fallbacks','exact_similarity_checks',
                                          'anchor_mapping_bytes','anchor_state_bytes')
                                config=(config and all(count(k)>=0 for k in required)
                                    and count('anchor_calls')==sum(count(k) for k in ('anchor_no_anchor','anchor_ineligible','anchor_eligible'))
                                    and count('anchor_eligible')==count('anchor_rejects')+count('anchor_fallbacks')
                                    and count('anchor_calls')==count('exact_similarity_checks')+count('anchor_rejects'))
                        if variant=='control' and config: reference=hashes
                        valid=bool(config and hashes is not None and reference is not None and hashes==reference
                                   and baseline_hash is not None and hashes['result']==baseline_hash)
                        row=dict(dataset=name,meta_path=path,epsilon=eps,mu=5,variant=variant,valid=int(valid),**metric)
                        if valid:
                            row.update(hinscan_compute_s=baseline_s,offline_compute_s=off['offline_compute_s'],
                                index_bytes=off['index_bytes'],online_compute_s=(float(metric['on_demand_fli_build_ms'])+float(metric['query_call_ms']))/1000)
                            if row['online_compute_s']>0: row['speedup_vs_hinscan']=baseline_s/row['online_compute_s']
                            if variant=='control': control_metric=row
                            elif anchor and control_metric is not None:
                                row['control_online_s']=control_metric['online_compute_s']
                                row['online_saved_vs_control_s']=control_metric['online_compute_s']-row['online_compute_s']
                                row['full_checks_avoided_vs_control']=int(control_metric['exact_similarity_checks'])-int(metric['exact_similarity_checks'])
                                row['anchor_extra_bytes']=int(metric['anchor_mapping_bytes'])+int(metric['anchor_state_bytes'])
                        else: errors.append(f'{name}/{path}/{variant}: execution/configuration/result validation failed')
                        rows.append(row)
                        save_json(qdir/f'{variant}-hashes.json',hashes)
                if digest(index)!=sha: raise RuntimeError('Index changed during query')
            except Exception as exc:
                errors.append(f'{name}: {exc}')
                for r in rows:
                    if r['dataset']==name:
                        r['valid']=0
                        for k in ('online_compute_s','speedup_vs_hinscan','control_online_s',
                                  'online_saved_vs_control_s','full_checks_avoided_vs_control'): r.pop(k,None)
                print('ERROR:',errors[-1],flush=True)
            finally:
                write_table(run/'runs.tsv',rows);write_table(run/'offline.tsv',offline)
    except Exception as exc:
        errors.append(str(exc));print('ERROR:',exc,flush=True)
    finally:
        expected_queries=len(variants)*sum(len(spec[2]) for spec in SPECS.values())
        if len(rows)!=expected_queries or len(offline)!=len(SPECS):
            errors.append(f'Incomplete run: {len(rows)}/{expected_queries} queries, {len(offline)}/{len(SPECS)} offline builds')
        write_table(run/'runs.tsv',rows);write_table(run/'offline.tsv',offline)
        keys=('dataset','meta_path','variant','valid','hinscan_compute_s','offline_compute_s','index_bytes','online_compute_s','speedup_vs_hinscan')
        if anchor:
            keys+=('control_online_s','online_saved_vs_control_s','anchor_prepare_ms','anchor_filter_ms',
                   'anchor_calls','anchor_eligible','anchor_rejects','anchor_cache_hits','anchor_resumed',
                   'anchor_entries_advanced','full_checks_avoided_vs_control','anchor_extra_bytes')
        write_table(run/'summary.tsv',[{k:r.get(k,'') for k in keys} for r in rows])
        save_json(run/'status.json',dict(all_passed=not errors,errors=errors,query_runs=len(rows),offline_runs=len(offline)))
        archive=Path(str(run)+'.tar.gz')
        with tarfile.open(archive,'w:gz') as tar:
            for p in sorted(run.rglob('*')):
                if p.is_file() and p.suffix in ('.log','.time','.json','.tsv'):
                    tar.add(p,arcname=str(Path(run.name)/p.relative_to(run)))
        print('Archive:',archive,flush=True)
        print('Completed' if not errors else 'Completed with errors; inspect status.json',flush=True)
    return int(bool(errors))

if __name__=='__main__':
    raise SystemExit(main())
