import struct
import unittest
import zlib
from probe_sgd5 import inflate, probe

class PayloadTests(unittest.TestCase):
    def compressed(self,raw):
        c=zlib.compressobj(wbits=-15)
        return c.compress(raw)+c.flush()
    def test_roundtrip(self):
        self.assertEqual(inflate(self.compressed(b'hello'*100),500),b'hello'*100)
    def test_wrong_size(self):
        with self.assertRaises(ValueError): inflate(self.compressed(b'hello'),4)
    def test_truncated(self):
        with self.assertRaises(ValueError): inflate(self.compressed(b'hello')[:-1],5)
    def test_trailing_data(self):
        with self.assertRaises(ValueError): inflate(self.compressed(b'hello')+b'junk',5)
    def test_excessive_output_size(self):
        with self.assertRaises(ValueError): inflate(b'',2**32)
    def group(self):
        return b'SGD5'+bytes(8)+struct.pack('<I',0)+struct.pack('<I4H',9,0,0,0,9)+b'\0'+bytes(28)
    def test_empty_arrays(self):
        d=self.group();r=probe(d,0,len(d))
        self.assertEqual(len(r['stage_arrays']),6)
        self.assertEqual(r['declared_effect_count'],0)
    def test_group_bound(self):
        d=self.group()
        with self.assertRaises(ValueError): probe(d,0,len(d)-1)
    def test_invalid_descriptor(self):
        d=bytearray(self.group());struct.pack_into('<I',d,12,1)
        with self.assertRaises(ValueError): probe(d,0,len(d))

if __name__=='__main__': unittest.main()
