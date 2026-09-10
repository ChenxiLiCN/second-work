import contextlib
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import run_server_hotspots as runner

class HotspotRunnerTests(unittest.TestCase):
    def exercise(self,fault=None):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);source=root/'20260910-213642-anchor-fixture';source.mkdir()
            (source/'status.json').write_text(json.dumps(dict(all_passed=True)))
            cases={name:dict(mu=5,queries=[[path,eps]]) for name,(path,eps) in runner.CASES.items()}
            (source/'environment.json').write_text(json.dumps(dict(version='anchor_v1',cases=cases)))
            h=lambda text:hashlib.sha256(text.encode()).hexdigest()
            with (source/'offline.tsv').open('w',newline='') as f:
                writer=csv.DictWriter(f,fieldnames=['dataset','index_bytes','sha256'],delimiter='\t');writer.writeheader()
                for name,(path,eps) in runner.CASES.items():
                    q=source/name/path;q.mkdir(parents=True)
                    (source/name/'base.bri').write_bytes(b'index')
                    writer.writerow(dict(dataset=name,index_bytes=5,sha256=h('index')))
                    (q/'control-hashes.json').write_text(json.dumps(dict(result=h('result'),roles=h('roles'))))
                    (q/'control.log').write_text('block_mode=11\nused_roundtrip_metadata=0\n')
            if fault=='source': (source/'status.json').write_text('{"all_passed":false}')
            calls=[]
            def measured(prefix,command,limit):
                exe=Path(command[0]).name;calls.append(exe)
                if exe=='verify_hotspot_diagnostics':
                    return dict(status=0,all_passed='0' if fault=='oracle' else '1')
                self.assertEqual(exe,'bri_diagnose_hotspots')
                out=Path(command[-1]);eps=command[-3]
                for kind in ('result','roles'):
                    (out/f'{kind}-{eps}-5.txt').write_text('bad' if fault=='roles' and kind=='roles' else kind)
                report=dict(diagnostic_only='1',stream_entries='10',generation_entries='5',
                            stream_calls='2',partial_calls='1',scan_entries='15')
                if fault=='counters': report['scan_entries']='999'
                (out/'hotspot-summary.log').write_text(''.join(f'{k}={v}\n' for k,v in report.items()))
                if fault!='report': (out/'hotspots.tsv').write_text('rank\tcanonical_vertex\n1\t0\n')
                if fault=='index': Path(command[1]).write_bytes(b'changed')
                return dict(status=124 if fault=='timeout' else 0,hotspot_diagnostic='1',block_mode='11',
                    anchor_filter='0',single_pass='1',lean_workspaces='1',cache_budget_mib='32',
                    used_roundtrip_metadata='0',witness_counts_built='0',adaptive_streaming_entries='10',
                    adaptive_posting_entries='5',adaptive_streaming_checks='2',single_pass_partial='1',
                    elapsed_s=999999,max_rss_kb='100')
            with patch.dict(os.environ,RESULT_ROOT=str(root)),patch.object(runner,'measured',measured), \
                 patch.object(runner,'checked'),contextlib.redirect_stdout(io.StringIO()):
                code=runner.main()
            self.assertEqual(code,int(fault is not None))
            run=next(p for p in root.iterdir() if p.is_dir() and '-hotspots-' in p.name)
            status=json.loads((run/'status.json').read_text())
            self.assertEqual(status['all_passed'],fault is None)
            if fault is None:
                self.assertEqual(calls.count('bri_diagnose_hotspots'),2)
                self.assertEqual(status['query_runs'],2)
                with (run/'summary.tsv').open(newline='') as f:
                    for row in csv.DictReader(f,delimiter='\t'):
                        self.assertEqual(row['valid'],'1');self.assertEqual(row['scan_entries'],'15')
                        self.assertNotIn('speedup_vs_hinscan',row)
                        self.assertNotIn('online_compute_s',row)
            with tarfile.open(str(run)+'.tar.gz') as tar:
                self.assertFalse(any(p.endswith(('.bri','.bin','.txt')) for p in tar.getnames()))
    def test_success(self): self.exercise()
    def test_roles(self): self.exercise('roles')
    def test_counters(self): self.exercise('counters')
    def test_missing_report(self): self.exercise('report')
    def test_timeout(self): self.exercise('timeout')
    def test_changed_index(self): self.exercise('index')
    def test_invalid_source(self): self.exercise('source')
    def test_oracle_failure(self): self.exercise('oracle')

if __name__=='__main__': unittest.main()
