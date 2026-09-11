#!/usr/bin/env python3
"""Hindsight shared-prefix feasibility diagnostic; Linux, two datasets, once."""
import math
import os
from pathlib import Path
import platform
import shutil
import tarfile
import tempfile
import time
from run_server_ablation import ROOT, checked, digest, fields, measured
from run_server_core import save_json, write_table, output_hashes
from run_server_hotspots import CASES, find_source

REFERENCE_KEYS=('exact_similarity_checks','candidate_vertices_emitted','candidate_posting_entries_read',
    'certificate_entries','adaptive_streaming_entries','adaptive_posting_entries','adaptive_hits',
    'adaptive_misses','adaptive_evictions','core_vertices','clusters')
COST_KEYS=('metadata_ms','capture_ms','batch_sort_ms','proof_ms','left_build_ms')
COUNT_KEYS=('calls','batches','groups','shared_calls','max_group','max_prefix','covered','positive',
    'negative','uncovered','mismatches','multi_prefix_groups','multi_prefix_covered','eligible_groups',
    'impossible_groups','proof_posting_entries','missing_insertions','left_build_entries',
    'baseline_posting_entries','covered_posting_entries','baseline_intersection_units','covered_intersection_units',
    'metadata_bytes','metadata_peak_bytes','peak_batch_bytes','peak_workspace_bytes')

def validate(metric,report,hashes,reference_hashes,reference):
    if metric.get('status')!=0 or hashes!=reference_hashes: return False
    config=dict(shared_group_diagnostic='1',block_mode='11',single_pass='1',lean_workspaces='1',
        resumable_neighborhoods='0',anchor_filter='0',cache_budget_mib='32',
        used_roundtrip_metadata='0',witness_counts_built='0')
    if any(metric.get(k)!=v for k,v in config.items()): return False
    if any(k not in reference or metric.get(k)!=reference[k] for k in REFERENCE_KEYS): return False
    if report.get('diagnostic_only')!='1' or report.get('hindsight_actual_checks')!='1': return False
    try:
        c={k:int(report[k]) for k in COUNT_KEYS}
        if any(v<0 for v in c.values()): return False
        t={k:float(report[k]) for k in COST_KEYS+('baseline_predicate_ms','covered_predicate_ms',
                                                'diagnostic_cost_ms','potential_predicate_balance_ms')}
        if any(not math.isfinite(v) for v in t.values()): return False
        if any(v<0 for k,v in t.items() if k!='potential_predicate_balance_ms'): return False
        if c['mismatches'] or c['calls']!=int(metric['exact_similarity_checks']): return False
        if c['covered']+c['uncovered']!=c['calls'] or c['positive']+c['negative']!=c['covered']: return False
        if not c['multi_prefix_covered']<=c['covered']<=c['shared_calls']<=c['calls']: return False
        if c['multi_prefix_groups']>c['groups']: return False
        if c['covered_posting_entries']>c['baseline_posting_entries']: return False
        if c['covered_intersection_units']>c['baseline_intersection_units']: return False
        if c['baseline_posting_entries']>int(metric['adaptive_streaming_entries'])+int(metric['adaptive_posting_entries']): return False
        if t['covered_predicate_ms']>t['baseline_predicate_ms']+1e-6: return False
        if not math.isclose(t['diagnostic_cost_ms'],sum(t[k] for k in COST_KEYS),rel_tol=1e-8,abs_tol=1e-6): return False
        if not math.isclose(t['potential_predicate_balance_ms'],
                            t['covered_predicate_ms']-t['diagnostic_cost_ms'],rel_tol=1e-8,abs_tol=1e-6): return False
        return True
    except (KeyError,ValueError,TypeError): return False

def main():
    results=Path(os.environ.get('RESULT_ROOT',ROOT/'server-results')).resolve()
    build=Path(os.environ.get('BUILD_DIR',ROOT/'build-linux-shared-groups')).resolve()
    jobs=int(os.environ.get('BUILD_JOBS','4'));limit=int(os.environ.get('LIMIT_SECONDS','7200'))
    if min(jobs,limit)<1: raise ValueError('Positive BUILD_JOBS/LIMIT_SECONDS required')
    results.mkdir(parents=True,exist_ok=True)
    run=Path(tempfile.mkdtemp(prefix=time.strftime('%Y%m%d-%H%M%S-shared-groups-'),dir=results))
    print('Results:',run,flush=True)
    rows=[];errors=[]
    try:
        source_run,source=find_source(results)
        print('Reusing verified indices:',source_run,flush=True)
        env=dict(version='shared_groups_v1',platform=platform.platform(),source_run=str(source_run),
            cases=CASES,repeats=1,
            scope='Mode 11 executes original comparisons and state updates. No raw conversion, index rebuild, materialized graph index or new clustering parameters.',
            grouping='Global posting-size order (ID ties), compressed prefix ranges over actual consecutive-left checks. No enumeration of all witness subsets. Group unions are never retained.',
            timing='All diagnostic timings are instrumented; none is a new algorithm online time. Covered predicate time excludes activation and is not necessarily saved after changing cache access patterns.',
            caveats='HINDSIGHT: actual future checks in a batch are known to grouping. This is favourable selection, not a deployable scheduler or universal upper bound. Extra grouping can pollute CPU caches. No end-to-end speedup reported.',
            memory='Ordered witness copy O(half-path incidences), vertex offsets/ranks, two temporary bitmaps, undo stack and one check batch; no whole-run edge trace.',
            cost='metadata+capture+batch_sort+proof+left_build; proof excludes left_build. Independent lazy left reconstruction is charged even though production already activated a left row. Positive balance is only an opportunity signal, not proof of benefit.')
        env['source_sha256']={str(p.relative_to(ROOT)):digest(p) for base in (ROOT/'src',ROOT/'experiments')
            for p in base.rglob('*') if p.is_file() and p.suffix in ('.cpp','.h','.py')}
        env['source_sha256']['CMakeLists.txt']=digest(ROOT/'CMakeLists.txt')
        env['reused_indices']={name:dict(path=str(s['index']),sha256=s['sha'],
             index_bytes=s['index'].stat().st_size) for name,s in source.items()}
        save_json(run/'environment.json',env)
        for name,s in source.items():
            if digest(s['index'])!=s['sha']: raise RuntimeError(f'{name}: input index hash mismatch')
        checked(['cmake','-S',ROOT,'-B',build,'-DCMAKE_BUILD_TYPE=Release','-DCMAKE_CXX_COMPILER=g++'],run/'configure.log')
        checked(['cmake','--build',build,'--parallel',jobs,'--target',
                 'bri_diagnose_shared_groups','verify_shared_groups'],run/'build.log')
        checked(['python3','-B','-m','unittest','discover','-s',ROOT/'experiments',
                 '-p','test_server_shared_groups.py'],run/'script-tests.log')
        with tempfile.TemporaryDirectory(prefix='oracle-',dir=run) as tmp:
            oracle=measured(run/'oracle',[build/'bin/verify_shared_groups',tmp],limit)
        if oracle['status']!=0 or oracle.get('all_passed')!='1': raise RuntimeError('Shared-group oracle failed')
        for name,(path,eps) in CASES.items():
            case=run/name;case.mkdir();s=source[name]
            try:
                reference=fields(source_run/name/path/'control.log')
                if any(k not in reference for k in REFERENCE_KEYS):
                    raise RuntimeError('Retained control log lacks execution counters')
                save_json(case/'reference-counters.json',{k:reference.get(k) for k in REFERENCE_KEYS})
                print(f'[{name}] shared-prefix diagnostic (once); NOT an online speed benchmark',flush=True)
                with tempfile.TemporaryDirectory(prefix='output-',dir=case) as tmp:
                    out=Path(tmp)
                    metric=measured(case/'diagnostic',[build/'bin/bri_diagnose_shared_groups',
                        s['index'],path,eps,'5',out],limit)
                    hashes=output_hashes(out,eps) if metric['status']==0 else None
                    report=fields(out/'shared-groups.log') if (out/'shared-groups.log').is_file() else {}
                    if report: shutil.copy2(out/'shared-groups.log',case/'shared-groups.log')
                save_json(case/'output-hashes.json',hashes);save_json(case/'metrics.json',metric)
                valid=validate(metric,report,hashes,s['hashes'],reference) and digest(s['index'])==s['sha']
                row=dict(dataset=name,meta_path=path,epsilon=eps,mu=5,valid=int(valid),
                         diagnostic_elapsed_s=metric.get('elapsed_s',''),
                         diagnostic_peak_rss_kb=metric.get('max_rss_kb',''),**report)
                if valid:
                    calls=int(report['calls'])
                    row['proof_coverage_fraction']=int(report['covered'])/calls if calls else 0
                    print(f"[{name}] covered={report['covered']}/{calls}; cost={float(report['diagnostic_cost_ms'])/1000:.3f}s; attributed predicate work={float(report['covered_predicate_ms'])/1000:.3f}s (not speedup)",flush=True)
                else: errors.append(f'{name}: proof/output/counters/index validation failed')
                rows.append(row)
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
