"""Build the experimental real-VPI counter host and demand cocotb XML results."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from pyslang_lower import Lowerer,cpp_header
from vpi_results import check_results
from vpi_runtime import environment
from vpi_provenance import digest, verify, capture


def build_candidate(candidate, folder, model):
    # This candidate consumes the flat IR without local coverage annotations.
    for instructions in model['programs']:
        for instruction in instructions:
            instruction.pop('line', None)
    header = folder / 'design.hpp'
    header.write_text(cpp_header(model).replace('resize(', 'bridge_resize('), encoding='utf-8')
    commands = [
        ['cmake', '-S', str(candidate), '-B', str(folder), '-DCMAKE_BUILD_TYPE=Debug',
         f'-DMINISIM_VPI_DESIGN_DIR={folder}'],
        ['cmake', '--build', str(folder), '--parallel', '1', '--target', 'minisim_vpi_counter'],
    ]
    (folder / 'build-commands.json').write_text(json.dumps(commands, indent=2))
    inputs = [
        candidate / 'CMakeLists.txt', candidate / 'tests/vpi_counter_host.cpp',
        header, folder / 'input.sv', Path(__file__).resolve(),
        ROOT / 'tools/pyslang_lower.py', ROOT / 'tests/cocotb_minisim_counter.py',
        *(p for p in sorted((candidate / 'src').iterdir()) if p.is_file()),
    ]
    before = {p: digest(p) for p in inputs}
    if before[folder / 'input.sv'] != model['source_sha256']:
        raise RuntimeError('RTL changed after lowering')
    for name, command in zip(('configure', 'build'), commands, strict=True):
        with (folder / f'{name}.log').open('x') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
    if any(digest(path) != expected for path, expected in before.items()):
        raise RuntimeError('candidate build inputs changed')
    binary = folder / 'minisim_vpi_counter'
    capture(binary, inputs, folder / 'provenance.json')
    return binary

def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,default=ROOT/'build/cocotb-minisim')
    p.add_argument('--clean-only',action='store_true');p.add_argument('--mutant-only',action='store_true')
    p.add_argument('--cmake-candidate', type=Path,
                   help='build both cases with an upstream scheduler checkout')
    a = p.parse_args()
    candidate = a.cmake_candidate.resolve() if a.cmake_candidate else None
    out = a.output.resolve()
    if candidate:
        if out == ROOT / 'build/cocotb-minisim' or a.clean_only or a.mutant_only:
            p.error('--cmake-candidate requires a fresh --output and runs both cases')
        clean = (candidate / 'examples/counter.sv').read_text()
        if clean.count('cnt + 1') != 1:
            raise RuntimeError('expected exactly one counter increment site')
        out.mkdir(parents=True)
    else:
        clean = (ROOT / 'examples/counter.sv').read_text()
        out.mkdir(parents=True, exist_ok=True)
    results = {}
    cases = [('clean', clean)]
    if not a.clean_only:
        cases.append(('increment2', clean.replace('cnt + 1', 'cnt + 2')))
    if a.mutant_only:
        results=json.loads((out/'summary.json').read_text())['results']
        if results['clean']['rtl_sha256']!=hashlib.sha256(clean.encode()).hexdigest():raise RuntimeError('Stale clean evidence')
        cases=[('increment2',clean.replace('cnt + 1','cnt + 2'))]
    for name,text in cases:
        folder=out/name;folder.mkdir(exist_ok=True);source=folder/'input.sv';source.write_text(text)
        model = Lowerer(source).lower()
        (folder / 'design.json').write_text(json.dumps(model, indent=2))
        if candidate:
            binary = build_candidate(candidate, folder, model)
            if name == 'increment2':
                clean_header = (out / 'clean/design.hpp').read_bytes()
                mutant_header = (folder / 'design.hpp').read_bytes()
                if (clean_header.count(b'mk(1ULL, 32)') != 1 or
                        clean_header.replace(b'mk(1ULL, 32)', b'mk(2ULL, 32)') != mutant_header):
                    raise RuntimeError('expected a single generated increment mutation')
            verify(folder / 'provenance.json')
        else:
            (folder / 'design.hpp').write_text(cpp_header(model))
            binary = folder / 'simulator'
            cmd = ['g++', '-std=c++20', '-O2', '-rdynamic', '-I', str(ROOT / 'src'),
                   '-I', str(folder), '-I', '/usr/include/iverilog',
                   str(ROOT / 'tests/vpi_counter_main.cpp'), '-ldl', '-o', str(binary)]
            with (folder / 'build.log').open('w') as log:
                subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        xml, trace = folder / 'results.xml', folder / 'trace.json'
        xml.unlink(missing_ok=True)
        trace.unlink(missing_ok=True)
        env = environment('counter', 'cocotb_minisim_counter', [ROOT / 'tests'], xml,
                          {'MINISIM_TRACE_PATH': str(trace)})
        with (folder / 'simulation.log').open('w') as log:
            subprocess.run([binary], cwd=folder, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True, timeout=90)
        if candidate:
            verify(folder / 'provenance.json')
            if name == 'increment2':
                verify(out / 'clean/provenance.json')
        expected_failure = None if name == 'clean' else 'counter mismatch sample 1: 2 != 1'
        verdict = check_results(xml, expected_failure=expected_failure)
        failures = verdict['failures']
        rows = json.loads(trace.read_text())
        if name == 'clean' and len(rows) != 96:
            raise RuntimeError('incomplete clean cocotb trace')
        if candidate and name == 'increment2' and rows != [
            {'cycle': 0, 'actual': 0, 'expected': 0},
            {'cycle': 1, 'actual': 2, 'expected': 1},
        ]:
            raise RuntimeError('unexpected mutant trace')
        results[name]=dict(samples=len(rows),failures=failures,expected_failure=name!='clean',
                           rtl_sha256=hashlib.sha256(text.encode()).hexdigest())
        print(name,results[name],flush=True)
    (out/'summary.json').write_text(json.dumps(dict(results=results,scope='experimental flat counter VPI host; no general RTL/VPI claim'),indent=2))

if __name__=='__main__':main()
