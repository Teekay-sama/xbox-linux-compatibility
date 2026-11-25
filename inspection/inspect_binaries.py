import json, struct, pathlib

root = pathlib.Path(__file__).resolve().parent.parent
def inspect(path):
    b = path.read_bytes()
    def u16(o): return struct.unpack_from('<H', b, o)[0]
    def u32(o): return struct.unpack_from('<I', b, o)[0]
    pe = u32(60)
    if b[:2] != b'MZ' or b[pe:pe+4] != b'PE\0\0':
        return {'file': path.name, 'error': 'Not a standard PE image'}
    opt = pe+24
    is64 = u16(opt)==0x20b
    sections=[]
    for i in range(u16(pe+6)):
        o=opt+u16(pe+20)+i*40
        sections.append((u32(o+12),u32(o+8),u32(o+16),u32(o+20)))
    def offset(rva):
        if rva < u32(opt+60): return rva
        for va,vs,rs,raw in sections:
            if va <= rva < va+max(vs,rs):
                if rva-va >= rs: raise ValueError('RVA has no file data')
                return raw+rva-va
        raise ValueError('Unmapped RVA '+hex(rva))
    def string(rva):
        o=offset(rva); end=b.find(b'\0',o,min(len(b),o+4096))
        return b[o:end if end>=0 else o+256].decode('ascii','replace')
    dd=opt+(112 if is64 else 96)
    imports={}; delayed={}
    def thunks(rva):
        result=[]; o=offset(rva); step=8 if is64 else 4
        for i in range(100000):
            v=int.from_bytes(b[o+i*step:o+(i+1)*step],'little')
            if not v: break
            result.append('ordinal:'+str(v&65535) if v & (1<<(step*8-1)) else string(v+2))
        return result
    if u32(dd+8):
        o=offset(u32(dd+8))
        for i in range(10000):
            t,stamp,chain,name,iat=struct.unpack_from('<5I',b,o+i*20)
            if not any((t,stamp,chain,name,iat)): break
            imports[string(name)]=thunks(t or iat)
    if u32(dd+13*8):
        o=offset(u32(dd+13*8))
        for i in range(10000):
            fields=struct.unpack_from('<8I',b,o+i*32)
            if not any(fields): break
            if fields[0]&1: delayed[string(fields[1])]=thunks(fields[4])
    exports=[]; total=0
    if u32(dd):
        o=offset(u32(dd)); total=u32(o+20); count=u32(o+24)
        names=offset(u32(o+32)) if count else 0
        exports=[string(u32(names+i*4)) for i in range(count)]
    return dict(file=path.name,machine=hex(u16(pe+4)),pe64=is64,subsystem=u16(opt+68),imports=imports,delay_imports=delayed,export_count=total,exports=exports)

results=[]
for path in sorted(root.glob('*.dll')):
    try: results.append(inspect(path))
    except Exception as e: results.append({'file':path.name,'error':str(e)})
with (root/'gameos.xvd').open('rb') as f: h=f.read(4096)
xvd={'flags':hex(struct.unpack_from('<I',h,0x208)[0]),'odk_keyslot':struct.unpack_from('<I',h,0x49c)[0],'wrapped_key_material_present':any(h[0x34c:0x36c])}
out={'xvd':xvd,'dlls':results}
(root/'inspection'/'binary-report.json').write_text(json.dumps(out,indent=2),encoding='utf-8')
print('XVD:',xvd)
for r in results:
    if 'error' in r: print(r); continue
    print(r['file'],r['machine'],'exports:',r['export_count'],'imports:',', '.join(r['imports']),'delay:',', '.join(r['delay_imports']))
    if r['file'] in ['XboxBvhBuilder.dll','amd_fidelityfx_loader_xbox.dll','amd_fidelityfx_upscaler_xbox.dll','Bink2Scarlett.dll']:
        print('  Export sample:',', '.join(r['exports'][:20]))
