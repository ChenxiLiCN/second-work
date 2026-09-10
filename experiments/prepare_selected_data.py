"""Lossless format adapters for the three selected datasets; never edits raw files."""
import json
from array import array
from pathlib import Path
from run_server_ablation import digest

SPECS = {
    'yelp': ('raw/hin_text/yelp', None,
             [('U-R-B-R-U', '0.9')]),
    'imdb_large': ('raw/entity_relation/imdb_large',
        ['movie','actor','cinematographer','composer','costume','designer','director','editor','producer','writer'],
        [('movie-actor-movie', '0.5')]),
    'foursquare': ('raw/entity_relation/foursquare',
        ['users','venues','dates','cities','categories'],
        [('user-venue-user', '0.5')]),
}

def schema(path):
    tokens=path.read_text(encoding='utf-8').split()
    at=0
    def take():
        nonlocal at
        value=tokens[at]; at+=1
        return value
    types=[(take(),int(take())) for _ in range(int(take()))]
    relations=[tuple(int(take()) for _ in range(3)) for _ in range(int(take()))]
    if any(n<0 for _,n in types) or any(a<0 or b<0 or a>=len(types) or b>=len(types) or m<0 for a,b,m in relations):
        raise ValueError('Invalid schema')
    return types,relations

def prepare(name, data_root, output):
    raw=Path(data_root)/SPECS[name][0]
    entities=SPECS[name][1]
    types,relations=schema(raw/'base.txt')
    output=Path(output)
    output.mkdir(parents=True,exist_ok=False)
    (output/'edge').mkdir()
    report=dict(dataset=name,source=str(raw.resolve()),source_sha256={'base.txt':digest(raw/'base.txt')},
                policy='Preserve every entity and relation record, including duplicate relations. Preserve local IDs when possible; bijectively remap sparse IDs with mapping files. No sampling or filtering.',
                original_types=types,entity_rows=[],relations=[])
    id_maps=[]
    report['id_remappings']=[]
    if entities is not None:
        if len(entities)!=len(types): raise ValueError('Unexpected type count')
        for tid,((type_name,count),filename) in enumerate(zip(types,entities)):
            p=raw/f'entity_{filename}.txt'
            ids=array('Q')
            with p.open(encoding='utf-8') as f:
                for line in f:
                    if not line.strip(): continue
                    original_id=int(line.split()[0])
                    if original_id<0: raise ValueError(f'Negative entity ID: {p}')
                    ids.append(original_id)
            if len(ids)!=count: raise ValueError(f'Entity count mismatch: {p}: {len(ids)} != {count}')
            if all(v<count for v in ids):
                seen=bytearray(count)
                for v in ids:
                    if seen[v]: raise ValueError(f'Duplicate entity ID: {p}: {v}')
                    seen[v]=1
                id_maps.append(None)
                del seen
            else:
                mapping={original:i for i,original in enumerate(sorted(ids))}
                if len(mapping)!=count: raise ValueError(f'Duplicate entity IDs: {p}')
                id_maps.append(mapping)
                map_file=output/f'type-{tid}-id-map.txt'
                with map_file.open('w',encoding='utf-8') as f:
                    for original,local in mapping.items(): f.write(f'{local}\t{original}\n')
                report['id_remappings'].append(dict(type=type_name,file=map_file.name,sha256=digest(map_file)))
            report['entity_rows'].append(len(ids))
            del ids
            report['source_sha256'][p.name]=digest(p)
    counts=[n for _,n in types]
    normalized=[]
    for rid,(a,b,declared) in enumerate(relations):
        p=raw/'edge'/f'{rid}.txt' if entities is None else raw/f'relation_{types[a][0]}_{types[b][0]}.txt'
        report['source_sha256'][str(p.relative_to(raw))]=digest(p)
        actual=0
        with p.open(encoding='utf-8') as src, (output/'edge'/f'{rid}.txt').open('w',encoding='utf-8',newline='\n') as dst:
            if entities is None:
                if tuple(map(int,src.readline().split()))!=(a,b,declared): raise ValueError('Relation header mismatch')
            dst.write(f'{a} {b} {declared}\n')
            for line in src:
                if not line.strip(): continue
                fields=line.split()
                if len(fields)!=2: raise ValueError(f'Malformed relation: {p}')
                u,v=map(int,fields)
                if min(u,v)<0: raise ValueError('Negative endpoint')
                if entities is not None:
                    if id_maps[a] is not None:
                        if u not in id_maps[a]: raise ValueError(f'Undeclared entity in {p}: {u}')
                        u=id_maps[a][u]
                    if id_maps[b] is not None:
                        if v not in id_maps[b]: raise ValueError(f'Undeclared entity in {p}: {v}')
                        v=id_maps[b][v]
                if entities is not None and (u>=counts[a] or v>=counts[b]): raise ValueError(f'Out-of-range endpoint: {p}')
                counts[a]=max(counts[a],u+1);counts[b]=max(counts[b],v+1)
                dst.write(f'{u} {v}\n');actual+=1
        if actual!=declared: raise ValueError(f'Relation count mismatch: {p}: {actual} != {declared}')
        normalized.append((a,b,actual))
        report['relations'].append(dict(source_type=a,target_type=b,records=actual))
    if name=='yelp' and counts!=[522139,934695,16000]:
        raise ValueError(f'Unexpected Yelp domains: {counts}; manual audit needed')
    report['normalized_types']=[(t,counts[i]) for i,(t,_) in enumerate(types)]
    report['domain_changes']=[dict(type=t,before=n,after=counts[i]) for i,(t,n) in enumerate(types) if n!=counts[i]]
    lines=[str(len(types)),*[f'{t} {counts[i]}' for i,(t,_) in enumerate(types)],
           str(len(normalized)),*[' '.join(map(str,r)) for r in normalized]]
    (output/'base.txt').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    report['normalized_sha256']={str(p.relative_to(output)):digest(p) for p in output.rglob('*.txt')}
    return report
