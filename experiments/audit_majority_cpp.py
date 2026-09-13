"""Tiny end-to-end C++ audit against independent closed-neighborhood semantics.

Creates only temporary synthetic graphs. No real dataset argument is accepted.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import struct
import tempfile

from audit_semantics import (fixtures, full_path_rows, incidence, reference,
                             write_hin, read_result, read_roles, cluster_part)
from majority_group_index import MajorityIndex


def check_stored_groups(file, counts, relations):
    """Decode retained C++ CSR and compare it to the independently tested model."""
    data = file.read_bytes()
    header = struct.unpack_from('<11Q', data)
    assert header[5] == 2*len(relations)
    expected = MajorityIndex(counts,relations)
    at = 88
    for a,b,_ in relations:
        for source,target in [(a,b),(b,a)]:
            sa,tb,sn,tn,gc,mc = struct.unpack_from('<6Q',data,at)
            at += 48
            assert (sa,tb,sn,tn)==(source,target,counts[source],counts[target])
            offsets = struct.unpack_from(f'<{gc+1}Q',data,at)
            at += 8*(gc+1)
            members = struct.unpack_from(f'<{mc}I',data,at)
            at += 4*mc
            groups = tuple(members[offsets[i]:offsets[i+1]] for i in range(gc))
            assert groups == tuple(g.members for g in expected.directions[source,target].groups)
    assert at == len(data)


def execute(args,success=True):
    p = subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=60)
    if (p.returncode == 0) != success:
        raise AssertionError(f'Unexpected exit {p.returncode}: {args}\n{p.stdout}\n{p.stderr}')
    return dict(line.split('=',1) for line in p.stdout.splitlines() if '=' in line)


def synthetic_cases():
    for f in fixtures():
        counts,relations = incidence(f['n'],f['edges'],f.get('duplicate',False))
        yield f['name'],counts,relations,[(f.get('path','A-B-A'),f['epsilon'],f['mu'])],None
    pairs = [(u,b) for u in range(12) for b in range(4) if b != u%4]
    yield 'early', [12,4],[(0,1,pairs)],[('A-B-A','1',12)],'early'
    for k in range(3):
        counts = [12]*(k+1)+[3]
        rels = [(i,i+1,[(u,u) for u in range(12)]) for i in range(k)]
        rels.append((k,k+1,[(u,b) for u in range(12) for b in range(3) if b != u%3]))
        types = list(range(k+2))+list(range(k,-1,-1))
        path = '-'.join(chr(65+t) for t in types)
        yield f'scalar{k}',counts,rels,[(path,'0.5',2),(path,'1',12)],'scalar'
    f = next(f for f in fixtures() if f['name']=='hub_via_border_neighbors')
    counts,rels = incidence(f['n'],f['edges'])
    pairs = rels[0][2]+[(u,counts[1]) for u in range(14,20)]
    yield 'mixed',[20,counts[1]+1],[(0,1,pairs)],[('A-B-A','0.45',5)],'mixed'
    yield 'reverse',[1,5],[(0,1,[(0,u) for u in range(4)])],[('B-A-B','1',4)],None
    yield 'empty',[0,2],[(0,1,[])],[('A-B-A','1',2)],None
    yield 'ghost',[2,3,2],[(0,1,[(0,0),(0,2),(1,1),(1,2)]),(1,2,[(0,0),(1,1)])],[('A-B-C-B-A','1',2)],None
    rng = random.Random(916)
    for case in range(36):
        counts = [rng.randrange(1,7) for _ in range(3)]
        rels = [(a,b,[(u,v) for u in range(counts[a]) for v in range(counts[b])
                     if rng.random() < (0.2,0.5,0.8)[case%3]]) for a,b in [(0,1),(1,2),(2,0)]]
        path = ['A-B-A','A-B-C-B-A','A-B-C-A-C-B-A','A-C-B-A-B-A-B-C-A'][case%4]
        yield f'random{case}',counts,rels,[(path,'0.5',2),(path,'0.9',3),(path,'1',counts[0]+1)],None


def audit(bin_dir):
    tools = {}
    for name in ['mgi_build_index','mgi_query_index','bri_query_core_connectivity','hin_materialize','pscan_baseline']:
        exe = bin_dir/name
        if not exe.is_file(): exe = exe.with_suffix('.exe')
        if not exe.is_file(): raise AssertionError(f'Required executable not built: {name}')
        tools[name] = exe.resolve()
    tested = 0
    with tempfile.TemporaryDirectory(prefix='mgi-cpp-audit-') as scratch:
        root = Path(scratch)
        for name,counts,rels,queries,kind in synthetic_cases():
            base = root/name
            raw = base/'raw'
            write_hin(raw,counts,rels)
            index = base/'index'
            off = execute([tools['mgi_build_index'],raw,index])
            assert float(off['offline_compute_ms']) >= 0
            total = sum(float(off[k]) for k in ('adjacency_build_ms','roundtrip_prepare_ms','majority_prepare_ms'))
            assert abs(total-float(off['offline_compute_ms'])) < 0.001
            assert int(off['index_bytes']) == sum((index/f).stat().st_size for f in ['base.bri','groups.mgi'])
            check_stored_groups(index/'groups.mgi',counts,rels)
            before = {f:hashlib.sha256((index/f).read_bytes()).hexdigest() for f in ('base.bri','groups.mgi')}
            for qi,(path,eps,mu) in enumerate(queries):
                rows = full_path_rows(counts,rels,path)
                want = reference(rows,eps,mu)
                out = base/f'q{qi}'
                metric = execute([tools['mgi_query_index'],index,path,eps,mu,out])
                got = read_result(out/f'result-{eps}-{mu}.txt',len(rows))
                roles = read_roles(out/f'roles-{eps}-{mu}.txt',len(rows))
                assert got == cluster_part(want),(name,path,eps,mu,got,want)
                assert roles['roles'] == want['roles'] and roles['memberships'] == want['memberships']
                assert metric['semantics_version']=='hinscan_nonindependent_v1'
                assert metric['mu_counts_self']=='1'
                assert float(metric['online_compute_ms']) >= 0
                phases = sum(float(metric[k]) for k in ['layer_proof_ms','residual_prepare_ms','residual_scan_ms','result_merge_ms'])
                assert phases <= float(metric['online_compute_ms'])+0.1
                residual = int(metric['residual_vertices'])
                assert residual+int(metric['completed_core_vertices'])+int(metric['completed_noncore_vertices']) == len(rows)
                if kind=='early' or (kind=='scalar' and eps=='0.5'):
                    assert residual == 0 and int(metric['residual_half_expansion_entries']) == 0
                if kind=='scalar' and eps=='0.5': assert int(metric['accepted_groups']) > 0
                if kind=='mixed': assert residual==13 and int(metric['completed_core_vertices'])==6
                # Existing production PSCAN path, same external mu and graph.
                control = base/f'control{qi}'
                execute([tools['bri_query_core_connectivity'],index/'base.bri',path,eps,mu,control])
                assert read_result(control/f'result-{eps}-{mu}.txt',len(rows)) == got
                assert read_roles(control/f'roles-{eps}-{mu}.txt',len(rows)) == roles
                # Upstream supplies clusters, not the paper's complete role definition.
                if rows:
                    projection = base/f'projection{qi}'
                    execute([tools['hin_materialize'],raw,path,projection])
                    execute([tools['pscan_baseline'],projection,eps,mu-1,'output'])
                    assert read_result(projection/f'result-{eps}-{mu-1}.txt',len(rows)) == got
                tested += 1
            assert before == {f:hashlib.sha256((index/f).read_bytes()).hexdigest() for f in before}
            # Reusing an output directory must fail rather than overwrite the index.
            execute([tools['mgi_build_index'],raw,index],False)
            if name=='early':
                for path,eps,mu in [('A-B','1',2),('A-C-A','1',2),('A-B-A','0',2),('A-B-A','1',1),
                                   ('A-B-A','1',-2),('A-B-A','1','2x'),('A-B-A','1.2',2),
                                   ('A-B-A','18446744073709551617',2)]:
                    execute([tools['mgi_query_index'],index,path,eps,mu,base/'invalid'],False)
                payload = index/'groups.mgi'
                data = payload.read_bytes()
                payload.write_bytes(data[:-1])
                execute([tools['mgi_query_index'],index,'A-B-A','1',12,base/'corrupt'],False)
    return dict(all_passed=True,queries=tested,scope='synthetic C++ full clusters/roles plus normalized upstream clusters; no runtime benchmark')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir',required=True,type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.bin_dir),indent=2))
