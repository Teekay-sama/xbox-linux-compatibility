import struct
import unittest
from inspect_shaders import dxbc_at

class ContainerBoundsTests(unittest.TestCase):
    def fixture(self):
        return bytearray(b'DXBC'+bytes(16)+struct.pack('<IIII',1,48,1,36)+b'SHDR'+struct.pack('<I',4)+bytes(4))
    def test_valid_embedded_container(self):
        parsed=dxbc_at(b'prefix'+self.fixture(),6)
        self.assertEqual(parsed['size'],48)
        self.assertEqual(parsed['parts'][0]['tag'],'SHDR')
    def test_truncated_data(self):
        self.assertIsNone(dxbc_at(self.fixture()[:-1],0))
    def test_part_outside_container(self):
        b=self.fixture();struct.pack_into('<I',b,40,100)
        self.assertIsNone(dxbc_at(b,0))
    def test_part_overlapping_header(self):
        b=self.fixture();struct.pack_into('<I',b,32,20)
        self.assertIsNone(dxbc_at(b,0))
    def test_random_signature(self):
        self.assertIsNone(dxbc_at(b'DXBC'+bytes(100),0))

if __name__=='__main__': unittest.main()
