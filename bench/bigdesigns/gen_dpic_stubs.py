#!/usr/bin/env python3
"""gen_dpic_stubs.py difftest-dpic.h > dpic_stubs.inc
Empty definitions for every v_difftest_* DPI function XiangShan's difftest calls
(build/generated-src/difftest-dpic.h). TrapEvent is written by hand in xs_dpi.cpp;
InstrCommit prints pc/instr when XS_TRACE is set."""
import re, sys
src = open(sys.argv[1]).read()
for name, args in re.findall(r'extern "C" void (v_difftest_\w+)\s*\(([^)]*)\)', src):
    if name == 'v_difftest_TrapEvent': continue
    args = ' '.join(args.split())
    body = ''
    if name == 'v_difftest_InstrCommit':
        body = ' if (xs_trace()) printf("xs_dpi: commit pc %llx instr %08x\\n", (unsigned long long)io_pc, (unsigned)io_instr); '
    print(f'extern "C" void {name}({args}) {{{body}}}')
