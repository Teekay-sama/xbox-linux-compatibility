"""Read-only archive probe; does not decrypt, execute, or assume RPF9 field meanings."""
import argparse
import collections
import hashlib
import json
import math
import mmap
import pathlib
import struct

def dxbc_at(data, start):
    """Validate container bounds and non-overlapping chunks, not shader semantics."""
    remaining = len(data) - start
    if remaining < 32 or data[start:start+4] != b'DXBC':
        return None
    version, size, count = struct.unpack_from('<III', data, start+20)
    if version != 1 or not 1 <= count <= 4096 or not 32+4*count <= size <= remaining:
        return None
    parts = []
    spans = []
    for i in range(count):
        off = struct.unpack_from('<I', data, start+32+4*i)[0]
        if off < 32+4*count or off+8 > size:
            return None
        tag, length = struct.unpack_from('<4sI', data, start+off)
        if off+8+length > size or any(c < 32 or c > 126 for c in tag):
            return None
        spans.append((off, off+8+length))
        parts.append({'tag': tag.decode('ascii'), 'offset': off, 'size': length})
    spans.sort()
    if any(a[1] > b[0] for a,b in zip(spans,spans[1:])):
        return None
    return {'offset': start, 'size': size, 'parts': parts}

def inspect(path):
    with path.open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
        header = bytes(data[:64])
        report = {'path': str(path.resolve()), 'size': len(data),
                  'sha256': hashlib.sha256(data).hexdigest(),
                  'magic_hex': header[:4].hex(), 'magic_ascii': header[:4].decode('ascii','replace'),
                  'header_u32_le': {hex(i): struct.unpack_from('<I',header,i)[0] for i in range(0,len(header)-3,4)},
                  'header_field_meanings': 'Unverified; numeric words are not interpreted as counts or offsets.',
                  'entropy_samples': [], 'signatures': {}, 'dxbc_containers': []}
        for pos in sorted(set([64, 4096, 65536, len(data)//4, len(data)//2, 3*len(data)//4, max(0,len(data)-65536)])):
            block = data[pos:min(pos+65536,len(data))]
            freq = collections.Counter(block)
            entropy = -sum((n/len(block))*math.log2(n/len(block)) for n in freq.values()) if block else 0
            report['entropy_samples'].append({'offset':pos,'bytes':len(block),'bits_per_byte':round(entropy,5)})
        report['sgd5_candidates'] = []
        for sig in [b'DXBC',b'DXIL',b'SHDR',b'SHEX',b'FSSM',b'SGD5',b'NOBC',b'PS\x00\x09',b'\x03\x02\x23\x07',b'9FPR',b'\x28\xb5\x2f\xfd']:
            label = sig.decode('ascii') if all(32<=c<127 for c in sig) else sig.hex()
            count=0; samples=[]; pos=0
            while True:
                pos=data.find(sig,pos)
                if pos<0: break
                count+=1
                if len(samples)<16: samples.append(pos)
                if sig==b'DXBC':
                    parsed=dxbc_at(data,pos)
                    if parsed: report['dxbc_containers'].append(parsed)
                if sig==b'SGD5':
                    block=bytes(data[pos:pos+128])
                    report['sgd5_candidates'].append({'offset':pos,'aligned_4096':pos%4096==0,
                        'header_hex':block.hex(' '),
                        'header_u32_le':{hex(i):struct.unpack_from('<I',block,i)[0] for i in range(0,len(block)-3,4)},
                        'status':'Marker and raw header only; boundaries and field semantics unverified.'})
                pos+=1
            report['signatures'][label]={'count':count,'sample_offsets':samples}
        report['validated_dxbc_count']=len(report['dxbc_containers'])
        report['dxbc_part_counts']=dict(collections.Counter(p['tag'] for c in report['dxbc_containers'] for p in c['parts']))
        report['limits']=['Signature matches alone are not proof of a shader.',
                          'High entropy cannot distinguish compression from encryption.',
                          'DXBC validation checks structure only, not checksum or executable semantics.',
                          'No RPF directory decryption, decompression, or shader conversion performed.']
        return report

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('archive',type=pathlib.Path)
    ap.add_argument('--output',type=pathlib.Path,required=True)
    args=ap.parse_args()
    if args.archive.resolve()==args.output.resolve(): ap.error('Output must not overwrite the archive')
    report=inspect(args.archive)
    args.output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='dxbc_containers'},indent=2))
