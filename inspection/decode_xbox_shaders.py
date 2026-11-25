"""Inspect XDAT/XBCH/FSSM; never execute shader code.
Format hypothesis checked against input bounds and decompression results:
https://github.com/ivall/xcl/blob/main/docs/specs/2026-11-20-xcl-design.md
"""
import collections
import hashlib
import json
import mmap
import pathlib
import struct
import zlib
from inspect_shaders import dxbc_at
from probe_sgd5 import inflate, LIMIT

ROOT=pathlib.Path(__file__).resolve().parent.parent
CODE_END = struct.pack('<I', 0xbf9f0000)

def code_metadata(fssm, start, code):
    # Count only the contiguous suffix. Interior words can be literals or data.
    tail = len(code)
    while tail >= 4 and code[tail-4:tail] == CODE_END:
        tail -= 4
    descriptors = []
    for offset in range(0x100, start - 7, 0x88):
        tag = struct.unpack_from('<Q', fssm, offset)[0]
        if tag >> 16 != 0x000000C00002:
            break
        descriptors.append({'offset': offset, 'api_stage_id': (tag >> 8) & 255,
                            'hardware_stage_id': tag & 255})
    return {'stage_descriptors': descriptors, 'trailing_code_end_words': (len(code) - tail) // 4,
            'bytes_before_code_end_suffix': tail}

def bounded_zlib(blob,expected):
    if not 0<expected<=LIMIT: raise ValueError('Invalid FSSM output size')
    dec=zlib.decompressobj()
    raw=dec.decompress(blob,expected+1)
    if len(raw)!=expected or not dec.eof or dec.unused_data or dec.unconsumed_tail:
        raise ValueError('Zlib stream length/end mismatch')
    return raw

def decode(raw):
    container=dxbc_at(raw,0)
    if not container or container['size']!=len(raw): raise ValueError('Invalid DXBC bounds')
    part=next((p for p in container['parts'] if p['tag']=='DXIL'),None)
    if not part: raise ValueError('Missing DXIL')
    program=raw[part['offset']+8:part['offset']+8+part['size']]
    if len(program)<36 or program[24:28]!=b'XDAT': raise ValueError('Missing XDAT header')
    count,table=struct.unpack_from('<II',program,28)
    if count>1024 or table<36 or table+12*count>len(program): raise ValueError('Invalid XDAT table')
    entries=[];payloads=[];spans=[]
    for i in range(count):
        ident,off,size=struct.unpack_from('<III',program,table+i*12)
        if off<table+12*count or off+size>len(program): raise ValueError('Invalid XDAT entry bounds')
        spans.append((off,off+size))
        body=program[off:off+size]
        entry={'id':ident,'offset':off,'size':size,'magic_hex':body[:4].hex()}
        entries.append(entry)
        if body[:4]!=b'XBCH': continue
        if len(body)<24: raise ValueError('Truncated XBCH')
        version=struct.unpack_from('<I',body,4)[0]
        packed,expected=struct.unpack_from('<QQ',body,8)
        if version!=3 or packed+24!=len(body): raise ValueError('Unsupported XBCH or compressed size mismatch')
        fssm=bounded_zlib(body[24:],expected)
        if len(fssm)<208 or fssm[:4]!=b'FSSM': raise ValueError('Missing FSSM')
        ver,total=struct.unpack_from('<II',fssm,4)
        if ver!=0x101 or total+16!=len(fssm): raise ValueError('Unsupported FSSM version or size mismatch')
        relative,length=struct.unpack_from('<QQ',fssm,192)
        start=relative+16
        if start<208 or start+length>len(fssm) or not length or length%4:
            raise ValueError('Invalid native-code range')
        code=fssm[start:start+length]
        entry.update(xbch_version=version,compressed_bytes=packed,fssm_bytes=len(fssm),
                     fssm_version=ver,fssm_size_field=total,code_offset=start,code_bytes=length,
                     code_sha256=hashlib.sha256(code).hexdigest(),code_prefix_hex=code[:64].hex(' '))
        entry.update(code_metadata(fssm, start, code))
        payloads.append((fssm,code))
    spans.sort()
    if any(a[1]>b[0] for a,b in zip(spans,spans[1:])): raise ValueError('Overlapping XDAT entries')
    if not payloads: raise ValueError('No XBCH payload in XDAT')
    return {'program_kind':struct.unpack_from('<I',program,0)[0]>>16,'entries':entries},payloads

def main():
    manifest=json.loads((ROOT/'inspection'/'sgd5-report.json').read_text())
    records={s['payload_offset']:s for g in manifest['groups'] for a in g['stage_arrays'] for s in a['records']}
    report={'records':[],'errors':[]};kinds=collections.Counter();versions=collections.Counter();saved=set();total=0
    payload_counts = collections.Counter()
    padding_words = 0
    descriptor_records = 0
    out=ROOT/'inspection'/'native-shader-samples';out.mkdir(exist_ok=True)
    with (ROOT/'shaders.rpf').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as d:
        for candidate in manifest['compressed_candidates']:
            offset=candidate['offset']
            try:
                raw=inflate(d[offset+12:offset+12+candidate['compressed_bytes']],candidate['decompressed_bytes'])
                if hashlib.sha256(raw).hexdigest()!=candidate['sha256']: raise ValueError('Previous payload hash mismatch')
                record,payloads=decode(raw)
                record.update(archive_offset=offset,name=records[offset]['name'])
                report['records'].append(record)
                kinds[record['program_kind']]+=1
                payload_counts[(record['program_kind'], len(payloads))] += 1
                for entry in record['entries']:
                    if 'code_bytes' in entry:
                        total+=entry['code_bytes'];versions[(entry['fssm_version'],entry['fssm_bytes']==entry['fssm_size_field']+16)]+=1
                        padding_words += entry['trailing_code_end_words']
                        descriptor_records += bool(entry['stage_descriptors'])
                kind=record['program_kind']
                if kind not in saved and payloads:
                    saved.add(kind)
                    for i,(fssm,code) in enumerate(payloads):
                        stem=f'kind{kind}_{offset:x}_{i}'
                        (out/(stem+'.fssm')).write_bytes(fssm)
                        (out/(stem+'.code.bin')).write_bytes(code)
                        record.setdefault('saved_samples',[]).append(stem)
            except (ValueError,zlib.error,struct.error,KeyError) as e:
                report['errors'].append({'offset':offset,'error':str(e)})
    report['summary']={'processed':len(manifest['compressed_candidates']),'parsed':len(report['records']),
                       'errors':len(report['errors']),'program_kinds':dict(kinds),'native_code_bytes':total,
                       'containers_by_kind_and_payload_count':{str(k):v for k,v in payload_counts.items()},
                       'trailing_code_end_bytes':padding_words*4,
                       'payloads_with_stage_descriptors':descriptor_records,
                       'fssm_versions_and_size_matches':{str(k):v for k,v in versions.items()}}
    (ROOT/'inspection'/'native-shader-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report['summary'],indent=2));print('First errors:',report['errors'][:5])

if __name__=='__main__': main()
