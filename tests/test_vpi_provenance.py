from pathlib import Path
import tempfile,unittest,sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from vpi_provenance import capture,verify
class ProvenanceTests(unittest.TestCase):
    def test_identity_and_mutations(self):
        with tempfile.TemporaryDirectory() as d:
            r=Path(d);b=r/'sim';s=r/'rtl';h=r/'header';out=r/'record.json'
            for p in (b,s,h):p.write_bytes(p.name.encode())
            capture(b,[s,h],out);verify(out)
            for p in (b,s,h):
                old=p.read_bytes();p.write_bytes(old+b'changed')
                with self.subTest(path=p.name),self.assertRaises(ValueError):verify(out)
                p.write_bytes(old)
            verify(out)
    def test_empty_inputs_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'b';p.write_text('b')
            with self.assertRaises(ValueError):capture(p,[],Path(d)/'record')
    def test_missing_input_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'b';p.write_text('b')
            with self.assertRaises(FileNotFoundError):capture(p,[Path(d)/'missing'],Path(d)/'record')
if __name__=='__main__':unittest.main()
