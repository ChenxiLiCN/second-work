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
import run_server_resumable as runner

class ResumableRunnerTests(unittest.TestCase):
    def exercise(self,fault=None):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source={};calls=[]
            h=lambda s:hashlib.sha256(s.encode()).hexdigest()
            for name in runner.CASES:
                index=root/(name+'.bri');index.write_bytes(b'index')
                source[name]=dict(index=index,sha=h('index'),hashes=dict(result=h('result'),roles=h('roles')))
            def measured(prefix,command,limit):
                exe=Path(command[0]).name;calls.append(exe)
                if exe.startswith('verify_'):
                    return dict(status=0,all_passed='0' if fault=='oracle' else '1')
                mode='13' if exe.endswith('resumable') else '11'
                out=Path(command[-1]);eps=command[-3]
                for kind in ('result','roles'):
                    (out/f'{kind}-{eps}-5.txt').write_text('bad' if fault=='roles' and kind=='roles' else kind)
                m=dict(status=124 if fault=='timeout' else 0,block_mode=mode,
                    resumable_neighborhoods=str(int(mode=='13')),anchor_filter='0',single_pass='1',
                    lean_workspaces='1',cache_budget_mib='32',used_roundtrip_metadata='0',witness_counts_built='0',
                    on_demand_fli_build_ms='1000',query_call_ms='1000' if mode=='13' else '2000',
                    base_relation_load_ms='9999999',output_write_ms='9999999',elapsed_s=999999,
                    resume_replay_ms='10',resume_save_ms='20',adaptive_peak_bytes='1024',adaptive_pair_bound_bytes='24',
                    adaptive_posting_entries='10',adaptive_streaming_entries='20',
                    resume_checks='2' if mode=='13' else '0',adaptive_streaming_checks='2',
                    single_pass_complete='1',single_pass_partial='1',resume_partial_saves='1',
                    resume_partial_drops='0',resume_new_entries='20')
                if fault=='counters' and mode=='13': m['resume_partial_saves']='9'
                if fault=='budget': m['adaptive_peak_bytes']=str(32*1024*1024)
                if fault=='timer': m['query_call_ms']='nan'
                if fault=='mode': m['anchor_filter']='1'
                if fault=='index': Path(command[1]).write_bytes(b'changed')
                return m
            finder=patch.object(runner,'find_source',return_value=(root,source),
                                side_effect=RuntimeError('missing retained index') if fault=='source' else None)
            captured=io.StringIO()
            with patch.dict(os.environ,RESULT_ROOT=str(root)),finder,patch.object(runner,'measured',measured), \
                 patch.object(runner,'checked'),contextlib.redirect_stdout(captured):
                code=runner.main()
            run=next(p for p in root.iterdir() if p.is_dir() and '-resumable-' in p.name)
            status=json.loads((run/'status.json').read_text())
            self.assertEqual(code,int(fault is not None),str(status)+captured.getvalue())
            self.assertEqual(status['all_passed'],fault is None)
            if fault is None:
                self.assertEqual(calls.count('bri_query_core_lean'),2)
                self.assertEqual(calls.count('bri_query_core_resumable'),2)
                self.assertEqual(calls[-4:],['bri_query_core_lean','bri_query_core_resumable',
                                             'bri_query_core_resumable','bri_query_core_lean'])
                # The total is 3s vs 2s, NOT elapsed/read/write or nested replay/save.
                lines=(run/'summary.tsv').read_text().splitlines()
                keys=lines[0].split('\t')
                for line in lines[1:]:
                    row=dict(zip(keys,line.split('\t')))
                    self.assertEqual(float(row['online_compute_s']),3 if row['variant']=='control' else 2)
                    if row['variant']=='resumable': self.assertEqual(float(row['speedup_vs_control']),1.5)
            if fault in ('oracle','source'):
                self.assertFalse(any(c.startswith('bri_query') for c in calls))
            with tarfile.open(str(run)+'.tar.gz') as tar:
                self.assertFalse(any(p.endswith(('.bri','.bin','.txt')) for p in tar.getnames()))
    def test_success(self): self.exercise()
    def test_roles(self): self.exercise('roles')
    def test_counters(self): self.exercise('counters')
    def test_budget(self): self.exercise('budget')
    def test_timer(self): self.exercise('timer')
    def test_mode(self): self.exercise('mode')
    def test_timeout(self): self.exercise('timeout')
    def test_changed_index(self): self.exercise('index')
    def test_missing_source(self): self.exercise('source')
    def test_oracle_failure(self): self.exercise('oracle')

if __name__=='__main__': unittest.main()
