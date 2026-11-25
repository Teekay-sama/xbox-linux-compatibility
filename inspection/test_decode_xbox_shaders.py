import struct
import unittest
import zlib
from decode_xbox_shaders import decode, bounded_zlib

def fixture(code=b'\0\0\x81\xbf',bad_offset=False):
    f=bytearray(212)
    struct.pack_into('<4sII',f,0,b'FSSM',0x101,len(f)-16)
    struct.pack_into('<QQ',f,192,9999 if bad_offset else 192,len(code))
    f[208:]=code
    z=zlib.compress(f)
    xb=struct.pack('<4sIQQ',b'XBCH',3,len(z),len(f))+z
    program=bytearray(48)+xb
    struct.pack_into('<4sII',program,24,b'XDAT',1,36)
    struct.pack_into('<III',program,36,13,48,len(xb))
    raw=b'DXBC'+bytes(16)+struct.pack('<IIII',1,44+len(program),1,36)+b'DXIL'+struct.pack('<I',len(program))+program
    return raw

class XboxContainerTests(unittest.TestCase):
    def test_full_chain(self):
        r,blobs=decode(fixture())
        self.assertEqual(blobs[0][1],b'\0\0\x81\xbf')
        self.assertEqual(r['entries'][0]['code_bytes'],4)
    def test_code_range(self):
        with self.assertRaises(ValueError): decode(fixture(bad_offset=True))
    def test_truncated_container(self):
        with self.assertRaises(ValueError): decode(fixture()[:-1])
    def test_trailing_compressed_data(self):
        with self.assertRaises(ValueError): bounded_zlib(zlib.compress(b'abc')+b'x',3)
    def test_inflate_limit(self):
        with self.assertRaises(ValueError): bounded_zlib(zlib.compress(b'a'*1000),5)
    def test_bad_xdat_bounds(self):
        raw=bytearray(fixture());struct.pack_into('<I',raw,44+32,0xffffffff)
        with self.assertRaises(ValueError): decode(raw)

if __name__=='__main__': unittest.main()
