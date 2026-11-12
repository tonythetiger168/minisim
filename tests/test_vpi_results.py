from pathlib import Path
import tempfile,unittest,sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from vpi_results import check_results
class ResultTests(unittest.TestCase):
    def check(self,xml,**kwargs):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'results.xml';p.write_text(xml);return check_results(p,**kwargs)
    def test_pass(self):self.assertEqual(self.check('<testsuite><testcase/></testsuite>')['failures'],0)
    def test_non_vacuous(self):
        for xml in ('<testsuite/>','<testsuite><testcase/><testcase/></testsuite>'):
            with self.subTest(xml=xml),self.assertRaises(ValueError):self.check(xml)
    def test_failure_policy(self):
        xml='<testsuite><testcase><failure message="ALU mismatch"/></testcase></testsuite>'
        with self.assertRaises(ValueError):self.check(xml)
        self.assertTrue(self.check(xml,expected_failure='ALU mismatch')['expected_failure'])
        with self.assertRaises(ValueError):self.check(xml,expected_failure='counter mismatch')
    def test_error_and_skip(self):
        for tag in ('error','skipped'):
            with self.subTest(tag=tag),self.assertRaises(ValueError):self.check(f'<testsuite><testcase><{tag}/></testcase></testsuite>')
    def test_orphan_failure_cannot_satisfy_expected_mutant(self):
        xml='<testsuite><testcase/><failure message="ALU mismatch"/></testsuite>'
        with self.assertRaises(ValueError):self.check(xml,expected_failure='ALU mismatch')
    def test_nested_failure_cannot_satisfy_expected_mutant(self):
        xml='<testsuite><testcase><attachment><failure message="ALU mismatch"/></attachment></testcase></testsuite>'
        with self.assertRaises(ValueError):self.check(xml,expected_failure='ALU mismatch')
    def test_declared_counts_must_match_nodes(self):
        for attr,value in [('failures','1'),('errors','1'),('skipped','1'),('tests','0'),('tests','bad')]:
            with self.subTest(attr=attr,value=value),self.assertRaises(ValueError):
                self.check(f'<testsuite {attr}="{value}"><testcase/></testsuite>')
    def test_declared_counts_valid(self):
        xml='<testsuites tests="1" failures="0"><testsuite tests="1" failures="0" errors="0" skipped="0"><testcase/></testsuite></testsuites>'
        self.assertEqual(self.check(xml)['failures'],0)
if __name__=='__main__':unittest.main()
