"""Read-only source reuse and fail-closed comparison protocol."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


class DiagnosisTests(unittest.TestCase):
    def runner(self):
        self.assertIsNotNone(importlib.util.find_spec('run_server_majority_diagnosis'),
                             'majority diagnosis runner is not implemented')
        import run_server_majority_diagnosis
        return run_server_majority_diagnosis

    def test_comparison_rejects_missing_invalid_and_wrong_mode(self):
        r = self.runner()
        control = dict(status=0, semantics_version='hinscan_nonindependent_v1',
                       mu_counts_self='1', pscan_other_mu='4', block_mode='9',
                       online_compute_ms='10')
        majority = dict(control, algorithm='majority_layered_v1', fallback_block_mode='9',
                        residual_diagnostics_version='1', online_compute_ms='12')
        for key in r.COUNTERS + r.TIMERS:
            control[key] = '10'
            majority['residual_'+key] = '5'
        rows = r.compare(control, majority)
        self.assertEqual(next(x for x in rows if x['metric']=='exact_similarity_checks')['majority_to_control'], .5)
        for key, value in [('residual_exact_similarity_checks', None),
                           ('residual_core_ms', 'nan'), ('fallback_block_mode', '11'),
                           ('residual_witness_entries_read', '-1'), ('status', 1)]:
            broken = dict(majority)
            if value is None: broken.pop(key)
            else: broken[key] = value
            with self.subTest(key=key), self.assertRaises((ValueError, RuntimeError)):
                r.compare(control, broken)
        control['exact_similarity_checks'] = '0'
        self.assertEqual(next(x for x in r.compare(control, majority)
                              if x['metric']=='exact_similarity_checks')['majority_to_control'], '')

    def test_source_requires_complete_run_and_unchanged_both_indices(self):
        r = self.runner()
        with tempfile.TemporaryDirectory() as tmp:
            run = Path(tmp)
            def save(p, value):
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(json.dumps(value))
            save(run/'status.json', dict(all_passed=True, query_runs=3, offline_runs=3))
            save(run/'environment.json', dict(version='majority_v1', mu=5,
                 cases={n:[[p,e]] for n,(p,e) in r.CASES.items()}))
            for name,(path,eps) in r.CASES.items():
                index = run/name/'index'
                index.mkdir(parents=True)
                for f in ('base.bri','groups.mgi'): (index/f).write_bytes(f.encode())
                save(run/name/'index-hashes.json', {f:r.digest(index/f) for f in ('base.bri','groups.mgi')})
                ref = dict(result='a'*64, roles='b'*64)
                for label in ('control','majority'): save(run/name/path/(label+'-hashes.json'), ref)
                save(run/name/path/'baseline.json', dict(result_sha256=ref['result']))
            self.assertEqual(set(r.load_source(run)), set(r.CASES))
            first = next(iter(r.CASES))
            (run/first/'index/groups.mgi').write_bytes(b'changed')
            with self.assertRaises((ValueError, RuntimeError)): r.load_source(run)

    def test_diagnose_rejects_role_mismatch_and_index_mutation(self):
        r = self.runner()
        for fault in ('roles','index','exit'):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as tmp:
                case = Path(tmp)
                index = case/'index'
                index.mkdir()
                for f in ('base.bri','groups.mgi'): (index/f).write_bytes(b'index')
                refdir = case/'reference'
                refdir.mkdir()
                (refdir/'result-0.5-5.txt').write_text('result')
                (refdir/'roles-0.5-5.txt').write_text('roles')
                source = dict(index=index,reference=r.hashes(refdir,'0.5'),
                              index_hashes={f:r.digest(index/f) for f in ('base.bri','groups.mgi')})
                calls = []
                def fake(prefix,command,limit):
                    calls.append(Path(command[0]).name)
                    out = Path(command[-1])
                    (out/'result-0.5-5.txt').write_text('result')
                    (out/'roles-0.5-5.txt').write_text('wrong' if fault=='roles' else 'roles')
                    if fault=='index': (index/'groups.mgi').write_bytes(b'changed')
                    return dict(status=1 if fault=='exit' else 0)
                with patch.object(r,'measured',fake), patch.object(r,'compare') as comparison:
                    with self.assertRaises(RuntimeError):
                        r.diagnose(Path('bin'),source,case,'A-B-A','0.5',60)
                    comparison.assert_not_called()
                self.assertTrue(set(calls) <= {'bri_query_core_connectivity','mgi_query_index'})

    def test_failed_source_still_produces_error_archive(self):
        r = self.runner()
        import os
        import tarfile
        with tempfile.TemporaryDirectory() as tmp:
            with patch.dict(os.environ,{'RESULT_ROOT':tmp,'SOURCE_RUN':str(Path(tmp)/'missing')}),\
                    patch.object(r.platform,'system',return_value='Linux'), patch.object(r,'checked') as checked:
                self.assertEqual(r.main(),1)
                checked.assert_not_called()
            run = next(p for p in Path(tmp).iterdir() if p.is_dir())
            status = json.loads((run/'status.json').read_text())
            self.assertFalse(status['all_passed'])
            self.assertEqual(status['query_pairs'],0)
            with tarfile.open(str(run)+'.tar.gz') as archive:
                self.assertTrue(any(n.endswith('status.json') for n in archive.getnames()))
                self.assertFalse(any(n.endswith(('.bri','.mgi')) for n in archive.getnames()))


if __name__ == '__main__':
    unittest.main()
