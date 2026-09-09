import contextlib
import csv
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import run_server_core_diagnosis as runner

class DiagnosisTests(unittest.TestCase):
    def exercise(self, fault=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / '20260909-core-test'
            records = []
            for name in runner.DATASETS:
                case = source / name
                case.mkdir(parents=True)
                (case / 'enhanced.bri').write_bytes(b'fixture')
                for kind in ('result', 'roles'):
                    (case / f'{kind}-0.5-5.txt').write_text(kind)
                runner.save_json(case / 'combined-hashes.json', runner.output_hashes(case, '0.5'))
                records.append(dict(dataset=name, index='enhanced', status=0,
                                    sha256=runner.digest(case / 'enhanced.bri')))
            runner.write_table(source / 'offline.tsv', records)
            if fault == 'hash':
                (source / runner.DATASETS[0] / 'enhanced.bri').write_bytes(b'corrupt')
            calls = []
            def measured(prefix, command, limit):
                calls.append(Path(command[0]).name)
                group = int(prefix.name[1:])
                out = Path(command[-1])
                for kind in ('result', 'roles'):
                    (out / f'{kind}-0.5-5.txt').write_text(kind)
                if fault == 'roles' and group == 1:
                    (out / 'roles-0.5-5.txt').write_text('bad')
                return dict(status=124 if fault == 'timeout' and group == 1 else 0,
                            block_mode='9', cache_budget_mib='32', used_roundtrip_metadata='1',
                            profile_group=str(group), on_demand_fli_build_ms='1000', query_call_ms='2000')
            with patch.dict(os.environ, RESULT_ROOT=str(root), INDEX_SEARCH_ROOT=str(root)), \
                 patch.object(runner, 'checked'), patch.object(runner, 'measured', measured), \
                 contextlib.redirect_stdout(io.StringIO()):
                code = runner.main()
            run = next(root.glob('*-core-diagnosis-*'))
            if run.suffix == '.gz':
                run = Path(str(run)[:-7])
            status = json.loads((run / 'status.json').read_text())
            self.assertEqual(code, int(fault is not None))
            self.assertEqual(status['all_passed'], fault is None)
            self.assertFalse(any('build_index' in c for c in calls))
            if fault is None:
                self.assertEqual(len(calls), 6)
                with (run / 'timing.tsv').open(newline='') as f:
                    rows = list(csv.DictReader(f, delimiter='\t'))
                self.assertEqual(len(rows), 2)
                self.assertTrue(all(float(r['online_compute_s']) == 3 for r in rows))
                with (run / 'diagnostics.tsv').open(newline='') as f:
                    rows = list(csv.DictReader(f, delimiter='\t'))
                self.assertEqual(len(rows), 4)
                self.assertTrue(all('online_compute_s' not in r for r in rows))
            if fault == 'hash':
                self.assertFalse(calls)

    def test_success(self): self.exercise()
    def test_bad_hash(self): self.exercise('hash')
    def test_bad_roles(self): self.exercise('roles')
    def test_timeout(self): self.exercise('timeout')

if __name__ == '__main__':
    unittest.main()
