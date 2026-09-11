import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import run_server_shared_groups as runner

class SharedGroupRunnerTests(unittest.TestCase):
    def exercise(self,fault=None):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source={};calls=[]
            h=lambda s:hashlib.sha256(s.encode()).hexdigest()
            reference={k:'10' for k in runner.REFERENCE_KEYS}
            for name,(path,eps) in runner.CASES.items():
                q=root/name/path;q.mkdir(parents=True)
                (q/'control.log').write_text(''.join(f'{k}={v}\n' for k,v in reference.items()))
                index=root/(name+'.bri');index.write_bytes(b'index')
                source[name]=dict(index=index,sha=h('index'),hashes=dict(result=h('result'),roles=h('roles')))
            def measured(prefix,command,limit):
                exe=Path(command[0]).name;calls.append(exe)
                if exe=='verify_shared_groups':
                    return dict(status=0,all_passed='0' if fault=='oracle' else '1')
                self.assertEqual(exe,'bri_diagnose_shared_groups')
                out=Path(command[-1]);eps=command[-3]
                for kind in ('result','roles'):
                    (out/f'{kind}-{eps}-5.txt').write_text('bad' if fault=='roles' and kind=='roles' else kind)
                r={k:'0' for k in runner.COUNT_KEYS}
                r.update(diagnostic_only='1',hindsight_actual_checks='1',calls='10',batches='2',groups='3',
                    shared_calls='6',covered='4',positive='1',negative='3',uncovered='6',
                    baseline_posting_entries='10',covered_posting_entries='4',
                    baseline_intersection_units='10',covered_intersection_units='4')
                r.update({k:'1' for k in runner.COST_KEYS})
                r.update(baseline_predicate_ms='10',covered_predicate_ms='4',diagnostic_cost_ms='5',
                         potential_predicate_balance_ms='-1')
                if fault=='proof': r['mismatches']='1'
                if fault=='coverage': r['covered']='12'
                if fault=='hindsight': r['hindsight_actual_checks']='0'
                if fault=='cost': r['diagnostic_cost_ms']='1'
                if fault=='nan': r['proof_ms']='nan'
                if fault!='report': (out/'shared-groups.log').write_text(''.join(f'{k}={v}\n' for k,v in r.items()))
                if fault=='index': Path(command[1]).write_bytes(b'changed')
                m=dict(reference,shared_group_diagnostic='1',block_mode='11',single_pass='1',
                    lean_workspaces='1',resumable_neighborhoods='0',anchor_filter='0',cache_budget_mib='32',
                    used_roundtrip_metadata='0',witness_counts_built='0',
                    status=124 if fault=='timeout' else 0,elapsed_s=999999,max_rss_kb='100')
                if fault=='trace': m['candidate_vertices_emitted']='11'
                if fault=='mode': m['block_mode']='13'
                return m
            captured=io.StringIO()
            finder=patch.object(runner,'find_source',return_value=(root,source),
                side_effect=RuntimeError('missing source') if fault=='source' else None)
            with patch.dict(os.environ,RESULT_ROOT=str(root)),finder,patch.object(runner,'measured',measured), \
                 patch.object(runner,'checked'),contextlib.redirect_stdout(captured):
                code=runner.main()
            run=next(p for p in root.iterdir() if p.is_dir() and '-shared-groups-' in p.name)
            status=json.loads((run/'status.json').read_text())
            self.assertEqual(code,int(fault is not None),str(status)+captured.getvalue())
            self.assertEqual(status['all_passed'],fault is None)
            if fault is None:
                self.assertEqual(calls.count('bri_diagnose_shared_groups'),2)
                lines=(run/'summary.tsv').read_text().splitlines()
                keys=lines[0].split('\t')
                self.assertNotIn('online_compute_s',keys);self.assertNotIn('speedup_vs_hinscan',keys)
                for line in lines[1:]:
                    row=dict(zip(keys,line.split('\t')))
                    self.assertEqual(float(row['proof_coverage_fraction']),0.4)
                    self.assertEqual(float(row['potential_predicate_balance_ms']),-1)
            if fault in ('oracle','source'):
                self.assertNotIn('bri_diagnose_shared_groups',calls)
            with tarfile.open(str(run)+'.tar.gz') as tar:
                self.assertFalse(any(p.endswith(('.bri','.bin','.txt')) for p in tar.getnames()))
    def test_success_even_if_balance_negative(self): self.exercise()
    def test_roles(self): self.exercise('roles')
    def test_proof_mismatch(self): self.exercise('proof')
    def test_coverage_count(self): self.exercise('coverage')
    def test_actual_trace_changed(self): self.exercise('trace')
    def test_hindsight_label(self): self.exercise('hindsight')
    def test_nested_cost(self): self.exercise('cost')
    def test_nan(self): self.exercise('nan')
    def test_mode(self): self.exercise('mode')
    def test_timeout(self): self.exercise('timeout')
    def test_missing_report(self): self.exercise('report')
    def test_changed_index(self): self.exercise('index')
    def test_missing_source(self): self.exercise('source')
    def test_oracle_failure(self): self.exercise('oracle')

if __name__=='__main__': unittest.main()

