"""Bounded independent probe of SGD5 metadata and DEFLATE payloads.
Format reference: https://github.com/rpatty-dev/PercyStar/blob/main/src/sgd5.rs
No upstream code is bundled. Input is never executed or modified.
"""
import json, mmap, pathlib, struct, zlib, hashlib
from inspect_shaders import dxbc_at

ROOT=pathlib.Path(__file__).resolve().parent.parent
LIMIT=64*1024*1024

def inflate(blob, expected):
    if not 0 < expected <= LIMIT: raise ValueError('Invalid output size')
    dec=zlib.decompressobj(-15)
    result=dec.decompress(blob,expected+1)
    if len(result)!=expected or not dec.eof or dec.unused_data or dec.unconsumed_tail:
        raise ValueError('Incomplete stream, trailing bytes or size mismatch')
    return result

def probe(d,base,end):
    def unpack(fmt,p):
        if p<base or p+struct.calcsize(fmt)>end: raise ValueError('Out of bounds at '+hex(p))
        return struct.unpack_from(fmt,d,p)
    def u32(p): return unpack('<I',p)[0]
    if d[base:base+4]!=b'SGD5': raise ValueError('Missing marker')
    n=u32(base+12)
    if n>10000: raise ValueError('Descriptor count exceeds bound')
    p=base+16; descriptors=[]
    for i in range(n):
        size=u32(p); count=unpack('<H',p+12)[0]
        if size!=24+8*count or p+4+size>end: raise ValueError('Invalid descriptor size')
        descriptors.append({'offset':p,'size':size,'entries':count})
        p+=4+size
    size=u32(p); a,b,res,copy=unpack('<4H',p+4)
    if size!=9+24*(a+b) or size!=copy: raise ValueError('Invalid shared reflection size')
    p+=4+size
    out={'offset':base,'upper_bound_not_record_end':end,'descriptors':descriptors,
         'shared_reflection_records':a+b,'arrays_offset':p,'next_128_hex':d[p:p+128].hex(' ')}
    out['stage_arrays']=[]
    # Stage labels follow the public format hypothesis; names are kept verbatim.
    for stage in ['VS','PS','GS','DS','HS','CS']:
        count=u32(p);p+=4
        if count>100000: raise ValueError('Invalid shader count')
        records=[]
        for i in range(count):
            start=p;length=unpack('<B',p)[0];p+=1
            if not length or p+length>end: raise ValueError('Invalid name length')
            name=bytes(d[p:p+length]).rstrip(b'\0').decode('ascii');p+=length
            flag=unpack('<B',p)[0];size=u32(p+1);p+=5
            payload=p
            if size<12 or p+size>end or u32(p)!=0xddce7 or u32(p+4)!=size:
                raise ValueError('Invalid compressed wrapper at '+hex(p))
            p+=size+152
            reflection=u32(p);r,c,t,dup=unpack('<4H',p+4)
            if reflection!=9+24*(r+c) or dup!=reflection: raise ValueError('Invalid shader reflection')
            p+=4+reflection
            if p>end: raise ValueError('Record crosses group bound')
            records.append({'index':i,'name':name,'record_start':start,'record_end':p,
                            'payload_offset':payload,'wrapper_bytes':size,'flag_raw':flag})
        out['stage_arrays'].append({'stage_hypothesis':stage,'count':count,'records':records})
    out['shader_arrays_end']=p
    out['declared_effect_count']=u32(p)
    return out

def main():
    with (ROOT/'shaders.rpf').open('rb') as f,mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as d:
        bases=[];p=0
        while (p:=d.find(b'SGD5',p))>=0: bases.append(p);p+=4
        report={'groups':[],'compressed_candidates':[]}
        for i,base in enumerate(bases):
            try: report['groups'].append(probe(d,base,bases[i+1] if i+1<len(bases) else len(d)))
            except (ValueError,struct.error) as e: report['groups'].append({'offset':base,'error':str(e)})
        p=0;valid=0;parts={};sample_dir=ROOT/'inspection'/'shader-samples'
        while (p:=d.find(bytes.fromhex('e7 dc 0d 00'),p))>=0:
            entry={'offset':p};report['compressed_candidates'].append(entry)
            if p+12>len(d): entry['error']='Truncated header';p+=4;continue
            entry['header_words']=list(struct.unpack_from('<III',d,p))
            expected=entry['header_words'][2]
            try:
                if not 0<expected<=LIMIT: raise ValueError('Invalid output size')
                # Stream end is independently detected by the decompressor.
                source=d[p+12:min(p+12+min(LIMIT,expected*2+65536),len(d))]
                dec=zlib.decompressobj(-15);raw=dec.decompress(source,expected+1)
                if len(raw)!=expected or not dec.eof: raise ValueError('No complete DEFLATE stream with expected size')
                consumed=len(source)-len(dec.unused_data)
                raw=inflate(source[:consumed],expected)
                entry.update(compressed_bytes=consumed,decompressed_bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest(),magic_hex=raw[:4].hex())
                container=dxbc_at(raw,0)
                entry['dxbc']=container
                if container:
                    part=next((part for part in container['parts'] if part['tag']=='DXIL'),None)
                    if part and part['size']>=24:
                        h=part['offset']+8
                        ver,words,magic,dxver,off,bcsize=struct.unpack_from('<6I',raw,h)
                        bcstart=h+8+off
                        entry['dxil_program']={'version_raw':ver,'word_count':words,'magic_hex':hex(magic),
                            'dxil_version':dxver,'bitcode_offset':off,'bitcode_size':bcsize,
                            'bitcode_in_bounds':bcstart>=h+24 and bcstart+bcsize<=h+part['size'],
                            'bitcode_magic_hex':raw[bcstart:bcstart+4].hex()}
                if container and container['size']==len(raw):
                    valid+=1
                    for part in container['parts']: parts[part['tag']]=parts.get(part['tag'],0)+1
                    if valid<=3:
                        sample_dir.mkdir(exist_ok=True)
                        name=f'{p:010x}.dxbc';(sample_dir/name).write_bytes(raw);entry['sample_file']=name
            except (ValueError,zlib.error) as e: entry['error']=str(e)
            p+=4
        report['validated_dxbc_count']=valid;report['part_counts']=parts
        (ROOT/'inspection'/'sgd5-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print(json.dumps({'groups':[{k:v for k,v in g.items() if k not in ('descriptors','stage_arrays','next_128_hex')} | {'stages':[{k:v for k,v in s.items() if k!='records'} for s in g.get('stage_arrays',[])]} for g in report['groups']], 'candidate_count':len(report['compressed_candidates']),'validated_dxbc_count':valid,'part_counts':parts,'first_programs':[c.get('dxil_program') for c in report['compressed_candidates'][:3]]},indent=2))

if __name__=='__main__': main()
