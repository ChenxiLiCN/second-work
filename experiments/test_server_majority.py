"""Runner protocol tests, no real data or Linux commands."""
import json
import os
from pathlib import Path
import tempfile
import tarfile
import unittest
from unittest.mock import patch
import run_server_majority as runner


class MajorityRunnerTests(unittest.TestCase):
    def run_case(self,fault=None):
        calls=[]
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            def fake(prefix,command,limit):
                exe=Path(command[0]).name
                calls.append(command)
                if exe=='hin_materialize': return dict(status=0,materialize_ms='1000')
                if exe=='pscan_baseline':
                    self.assertEqual(command[-2],'4')
                    prefix.with_suffix('.log').write_text('Total time without IO: 2000000\n')
                    (Path(command[1])/'result-0.5-4.txt').write_text('same')
                    return dict(status=0)
                out=Path(command[-1])
                (out/'result-0.5-5.txt').write_text('same')
                (out/'roles-0.5-5.txt').write_text('wrong' if fault=='roles' and exe=='mgi_query_index' else 'roles')
                return dict(status=1 if fault=='exit' else 0,online_compute_ms='250',
                            semantics_version='wrong' if fault=='semantics' else 'hinscan_nonindependent_v1',
                            mu_counts_self='1',pscan_other_mu='5' if fault=='mu' else '4',
                            algorithm='majority_layered_v1',block_mode='9',fallback_block_mode='9')
            with patch.object(runner,'measured',fake):
                result=runner.benchmark_query(Path('bin'),root/'raw',root/'index',root/'query','A-B-A','0.5',60)
        self.assertEqual([Path(c[0]).name for c in calls],
                         ['hin_materialize','pscan_baseline','bri_query_core_connectivity','mgi_query_index'])
        return result

    def test_once_correct_mu_and_compute_only_ratio(self):
        r=self.run_case()
        self.assertEqual(r['hinscan_compute_s'],3)
        self.assertEqual(r['online_compute_s'],0.25)
        self.assertEqual(r['speedup_vs_hinscan'],12)

    def test_failure_role_semantics_or_mu_never_reports_speedup(self):
        for fault in ('roles','exit','semantics','mu'):
            with self.subTest(fault=fault),self.assertRaises(RuntimeError):self.run_case(fault)

    def test_bad_timers_rejected(self):
        for value in ('nan','inf','-1'):
            with self.assertRaises(RuntimeError):runner.seconds(dict(status=0,t=value),'t')
        with self.assertRaises(RuntimeError):runner.seconds(dict(status=1,t='1'),'t')

    def test_dataset_scope(self):
        self.assertEqual(set(runner.SPECS),{'yelp','imdb_large','foursquare'})
        self.assertEqual(sum(len(s[2]) for s in runner.SPECS.values()),3)

    def test_full_runner_archive_excludes_indexes_and_marks_failures(self):
        for failed in (False,True):
            with self.subTest(failed=failed),tempfile.TemporaryDirectory() as tmp:
                root=Path(tmp)
                for spec in runner.SPECS.values():
                    p=root/'data'/spec[0]
                    p.mkdir(parents=True)
                    (p/'base.txt').write_text('synthetic')
                def prepare(name,data,raw):
                    raw.mkdir()
                    (raw/'base.txt').write_text('synthetic')
                    return {'domain_changes':[]}
                builds=[]
                def measured(prefix,command,limit):
                    builds.append(command)
                    index=Path(command[-1]);index.mkdir()
                    (index/'base.bri').write_bytes(b'raw')
                    (index/'groups.mgi').write_bytes(b'groups')
                    return dict(status=0,offline_compute_ms='1000',index_bytes='9')
                def query(bin_dir,raw,index,directory,path,epsilon,limit):
                    if failed:raise RuntimeError('synthetic mismatch')
                    return dict(meta_path=path,epsilon=epsilon,mu=5,valid=1,
                                hinscan_compute_s=3,online_compute_s=0.25,speedup_vs_hinscan=12)
                env=dict(DATA_ROOT=str(root/'data'),RESULT_ROOT=str(root/'results'),
                         BUILD_DIR=str(root/'build'))
                with patch.dict(os.environ,env),patch.object(runner.platform,'system',return_value='Linux'),\
                     patch.object(runner,'checked'),patch.object(runner,'prepare',prepare),\
                     patch.object(runner,'measured',measured),patch.object(runner,'benchmark_query',query):
                    code=runner.main()
                self.assertEqual(code,int(failed))
                self.assertEqual(len(builds),3)
                run=next(p for p in (root/'results').iterdir() if p.is_dir())
                status=json.loads((run/'status.json').read_text())
                self.assertEqual(status['all_passed'],not failed)
                with tarfile.open(str(run)+'.tar.gz') as archive:
                    names=archive.getnames()
                self.assertTrue(any(n.endswith('summary.tsv') for n in names))
                self.assertFalse(any(n.endswith(('.bri','.mgi')) for n in names))


if __name__=='__main__':
    unittest.main()
