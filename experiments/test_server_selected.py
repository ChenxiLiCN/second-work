import contextlib
import csv
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import prepare_selected_data as adapter
import run_server_selected as runner

class AdapterTests(unittest.TestCase):
    def exercise(self, fault=None):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);raw=root/'raw';raw.mkdir()
            (raw/'base.txt').write_text('3\nmovie 2\nactor 1\ncostume 0\n2\n0 1 3\n0 2 0\n')
            (raw/'entity_movie.txt').write_text('0 x\n1 y\n')
            (raw/'entity_actor.txt').write_text('0 z\n')
            (raw/'entity_costume.txt').write_text('')
            (raw/'relation_movie_actor.txt').write_text('0 0\n0 0\n1 0\n' if fault is None else '9 0\n0 0\n1 0\n')
            (raw/'relation_movie_costume.txt').write_text('')
            spec=('raw',['movie','actor','costume'],[])
            before={p.name:adapter.digest(p) for p in raw.iterdir()}
            with patch.dict(adapter.SPECS,fixture=spec):
                if fault:
                    with self.assertRaises(ValueError): adapter.prepare('fixture',root,root/'out')
                else:
                    report=adapter.prepare('fixture',root,root/'out')
                    self.assertEqual(report['domain_changes'],[])
                    self.assertEqual((root/'out/edge/0.txt').read_text(),'0 1 3\n0 0\n0 0\n1 0\n')
                    self.assertEqual((root/'out/edge/1.txt').read_text(),'0 2 0\n')
            self.assertEqual(before,{p.name:adapter.digest(p) for p in raw.iterdir()})
    def test_preserve_duplicates_and_empty_type(self): self.exercise()
    def test_invalid_endpoint(self): self.exercise('endpoint')
    def test_unordered_and_sparse_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);raw=root/'raw';raw.mkdir()
            (raw/'base.txt').write_text('2\nmovie 2\nactor 2\n1\n0 1 2\n')
            (raw/'entity_movie.txt').write_text('1 y\n0 x\n')
            (raw/'entity_actor.txt').write_text('80 b\n10 a\n')
            (raw/'relation_movie_actor.txt').write_text('1 80\n0 10\n')
            with patch.dict(adapter.SPECS,fixture=('raw',['movie','actor'],[])):
                report=adapter.prepare('fixture',root,root/'out')
            self.assertEqual((root/'out/edge/0.txt').read_text(),'0 1 2\n1 1\n0 0\n')
            self.assertEqual(len(report['id_remappings']),1)

class SelectedRunnerTests(unittest.TestCase):
    anchor=False
    def exercise(self,fault=None):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for raw,_,_ in runner.SPECS.values():
                p=root/raw;p.mkdir(parents=True);(p/'base.txt').write_text('fixture')
            def prepare(name,data,out):
                out.mkdir();(out/'base.txt').write_text('fixture')
                return dict(domain_changes=[])
            calls=[]
            def measured(prefix,command,limit):
                exe=Path(command[0]).name;calls.append(exe)
                if exe in ('verify_adaptive_fli','verify_anchor_filter'): return dict(status=0,all_passed='0' if fault=='oracle' else '1')
                if exe=='bri_build_measured':
                    Path(command[-1]).write_bytes(b'index')
                    return dict(status=0,offline_compute_ms='2000',hin_load_ms='999999')
                if exe=='hin_materialize': return dict(status=124 if fault=='baseline' else 0,materialize_ms='3000')
                if exe=='pscan_baseline':
                    (prefix.with_suffix('.log')).write_text('Total time without IO: 4000000\n')
                    (Path(command[1])/f'result-{command[2]}-5.txt').write_text('result')
                    return dict(status=0)
                eps=command[-3];out=Path(command[-1]);latest=exe==('bri_query_core_anchor' if self.anchor else 'bri_query_core_lean')
                for kind in ('result','roles'):
                    (out/f'{kind}-{eps}-5.txt').write_text('wrong' if fault=='roles' and latest and kind=='roles' else kind)
                if fault=='index' and latest: Path(command[1]).write_bytes(b'changed')
                metric=dict(status=124 if fault=='timeout' and latest else 0,block_mode=str((12 if latest else 11) if self.anchor else (11 if latest else 10)),
                    used_roundtrip_metadata='0',cache_budget_mib='32',single_pass='1',
                    lean_workspaces=str(int(self.anchor or latest)),witness_counts_built='0',
                    on_demand_fli_build_ms='1000',query_call_ms='1500' if latest else '2000',
                    base_relation_load_ms='999999',output_write_ms='999999')
                if self.anchor:
                    metric.update(anchor_filter=str(int(latest)),anchor_calls='10' if latest else '0',
                        anchor_no_anchor='1',anchor_ineligible='2',anchor_eligible='7',
                        anchor_rejects='4',anchor_fallbacks='3',exact_similarity_checks='6' if latest else '10',
                        anchor_mapping_bytes='40',anchor_state_bytes='32')
                    if fault=='accounting' and latest: metric['anchor_eligible']='99'
                return metric
            with patch.dict(os.environ,DATA_ROOT=str(root),RESULT_ROOT=str(root/'results')), \
                 patch.object(runner,'prepare',prepare),patch.object(runner,'measured',measured), \
                 patch.object(runner,'checked'),contextlib.redirect_stdout(io.StringIO()):
                code=runner.main(anchor=self.anchor)
            run=next(p for p in (root/'results').iterdir() if p.is_dir())
            status=json.loads((run/'status.json').read_text())
            self.assertEqual(code,int(fault is not None))
            self.assertEqual(status['all_passed'],fault is None)
            if fault is None:
                self.assertEqual(calls.count('bri_build_measured'),3)
                self.assertEqual(status['query_runs'],6)
                with (run/'summary.tsv').open(newline='') as f: rows=list(csv.DictReader(f,delimiter='\t'))
                for row in rows:
                    self.assertEqual(float(row['hinscan_compute_s']),7)
                    self.assertEqual(float(row['offline_compute_s']),2)
                    self.assertEqual(float(row['online_compute_s']),2.5 if row['variant']=='latest' else 3)
                    if self.anchor and row['variant']=='latest':
                        self.assertEqual(float(row['online_saved_vs_control_s']),0.5)
                        self.assertEqual(int(row['full_checks_avoided_vs_control']),4)
                        self.assertEqual(int(row['anchor_extra_bytes']),72)
            if fault=='index':
                with (run/'summary.tsv').open(newline='') as f:
                    for row in csv.DictReader(f,delimiter='\t'):
                        self.assertEqual(row['valid'],'0')
                        self.assertEqual(row['speedup_vs_hinscan'],'')
                        self.assertEqual(row.get('online_saved_vs_control_s',''),'')
            with tarfile.open(str(run)+'.tar.gz') as archive:
                self.assertFalse(any(n.endswith(('.bri','.txt','.bin')) for n in archive.getnames()))
    def test_success(self): self.exercise()
    def test_roles(self): self.exercise('roles')
    def test_baseline(self): self.exercise('baseline')
    def test_timeout(self): self.exercise('timeout')
    def test_index(self): self.exercise('index')
    def test_oracle(self): self.exercise('oracle')

if __name__=='__main__':
    unittest.main()
