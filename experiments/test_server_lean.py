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
import run_server_lean as runner


class LeanRunnerTests(unittest.TestCase):
    def exercise(self, fault=None):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / '20260909-core-test'
            records = []
            for name in runner.DATASETS:
                case = source / name
                case.mkdir(parents=True)
                (case / 'legacy.bri').write_bytes(b'fixture')
                for kind in ('result', 'roles'):
                    (case / f'{kind}-0.5-5.txt').write_text(kind)
                expected = runner.output_hashes(case, '0.5')
                runner.save_json(case / 'combined-hashes.json', expected)
                runner.save_json(case / 'baseline.json', dict(materialize=dict(status=0),
                    pscan=dict(status=0), cluster_sha256=expected['result']))
                (case / 'materialize.log').write_text('materialize_ms=1000\n')
                (case / 'pscan.log').write_text('Total time without IO: 2000000\n')
                records.append(dict(dataset=name, index='legacy', status=0,
                    sha256=runner.digest(case / 'legacy.bri')))
            runner.write_table(source / 'offline.tsv', records)
            if fault == 'hash':
                (source / runner.DATASETS[0] / 'legacy.bri').write_bytes(b'bad')
            calls = []
            def measured(prefix, command, limit):
                calls.append(Path(command[0]).name)
                if prefix.name == 'oracle':
                    return dict(status=0, all_passed='0' if fault == 'oracle' else '1')
                out = Path(command[-1])
                for kind in ('result', 'roles'):
                    (out / f'{kind}-0.5-5.txt').write_text(kind)
                is_new = prefix.name == 'lean'
                if fault == 'roles' and is_new:
                    (out / 'roles-0.5-5.txt').write_text('wrong')
                return dict(status=124 if fault == 'timeout' and is_new else 0,
                    block_mode='11' if is_new else '10', single_pass='1', lean_workspaces=str(int(is_new)),
                    adaptive_hits='3', adaptive_misses='4', adaptive_evictions='0',
                    single_pass_complete='1', single_pass_partial='2', activation_calls='2',
                    exact_similarity_checks='5', candidate_posting_entries_read='9',
                    witness_counts_built='1' if fault == 'witness' and is_new else '0', witness_bound_checks='0',
                    certified_blocks='2' if fault == 'blocks' and is_new else '1', block_core_vertices='6',
                    adaptive_posting_entries='100', adaptive_streaming_entries='200', witness_entries_read='0',
                    used_roundtrip_metadata='1' if fault == 'metadata' else '0',
                    cache_budget_mib='32', on_demand_fli_build_ms='1000', query_call_ms='500' if is_new else '2000',
                    online_compute_ms='1500', index_load_ms='999999', output_write_ms='999999')
            with patch.dict(os.environ, RESULT_ROOT=str(root), INDEX_SEARCH_ROOT=str(root)), \
                 patch.object(runner, 'checked'), patch.object(runner, 'measured', measured), \
                 contextlib.redirect_stdout(io.StringIO()):
                code = runner.main()
            run = next(p for p in root.glob('*-lean-*') if p.is_dir())
            status = json.loads((run / 'status.json').read_text())
            self.assertEqual(code, int(fault is not None))
            self.assertEqual(status['all_passed'], fault is None)
            self.assertFalse(any('build_index' in c for c in calls))
            with tarfile.open(str(run) + '.tar.gz') as archive:
                self.assertFalse(any(n.endswith('.bri') for n in archive.getnames()))
            if fault is None:
                self.assertEqual(len(calls), 5)
                with (run / 'summary.tsv').open(newline='') as f:
                    rows = list(csv.DictReader(f, delimiter='\t'))
                self.assertEqual(len(rows), 4)
                for r in rows:
                    self.assertEqual(float(r['historical_hinscan_compute_s']), 3)
                    self.assertEqual(float(r['online_compute_s']), 3 if r['variant']=='control' else 1.5)
            if fault == 'hash':
                self.assertFalse(calls)

    def test_success(self): self.exercise()
    def test_hash(self): self.exercise('hash')
    def test_roles(self): self.exercise('roles')
    def test_timeout(self): self.exercise('timeout')
    def test_metadata(self): self.exercise('metadata')
    def test_oracle(self): self.exercise('oracle')
    def test_witness(self): self.exercise('witness')
    def test_blocks(self): self.exercise('blocks')


if __name__ == '__main__':
    unittest.main()
