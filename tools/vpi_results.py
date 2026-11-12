"""Strict, reusable cocotb result acceptance; failure expectations are explicit."""
from pathlib import Path
import xml.etree.ElementTree as ET
def check_results(path,expected_tests=1,expected_failure=None):
    if expected_tests<1:raise ValueError('non-empty testcase count required')
    tree=ET.parse(Path(path));tests=tree.findall('.//testcase')
    for suite in tree.iter():
        if suite.tag not in ('testsuite','testsuites'):continue
        for attribute,tag in (('tests','testcase'),('failures','failure'),('errors','error'),('skipped','skipped')):
            declared=suite.get(attribute)
            if declared is not None and (not declared.isdecimal() or int(declared)!=sum(1 for node in suite.iter(tag))):
                raise ValueError('declared XML counts disagree with result nodes')
    if len(tests)!=expected_tests:raise ValueError('missing or unexpected testcases')
    if tree.findall('.//error') or tree.findall('.//skipped'):raise ValueError('error/skipped testcase')
    failures=tree.findall('.//failure')
    parents={child:parent for parent in tree.iter() for child in parent}
    if any(parents.get(failure) not in tests for failure in failures):
        raise ValueError('failure must directly belong to a testcase')
    if expected_failure is None:
        if failures:raise ValueError('unexpected testcase failure')
    else:
        if not expected_failure or len(failures)!=1 or expected_failure not in ET.tostring(failures[0],encoding='unicode'):
            raise ValueError('expected scoreboard failure was not observed')
    return {'testcases':len(tests),'failures':len(failures),'expected_failure':expected_failure is not None}
