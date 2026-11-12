"""Lower real RTL through pyslang, execute generated Minisim IR, compare Icarus.

Tests a 4-bit counter, an 8-bit variant, and a +2 mutant. The mutant must match
its own RTL in Icarus AND differ from the clean trace, preventing a hard-coded demo.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from pyslang_lower import Lowerer, cpp_header


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                          check=True, timeout=120).stdout


def compare(source, folder):
    folder.mkdir(parents=True, exist_ok=True)
    model = Lowerer(source).lower()
    (folder / "design.json").write_text(json.dumps(model, indent=2))
    (folder / "design.hpp").write_text(cpp_header(model))
    with tempfile.TemporaryDirectory(prefix="minisim_frontend_") as temporary:
        work = Path(temporary)
        exe = work / "native"
        run(["g++", "-std=c++20", "-O2", "-I", str(ROOT / "src"), "-I", str(folder),
             str(ROOT / "tests/frontend_counter_harness.cpp"), "-o", str(exe)], work)
        native = run([str(exe)], work).splitlines()
        statements = ["module tb; reg clk, rst; wire [" +
                      str(model["signals"]["cnt"] - 1) + ":0] cnt; counter dut(clk,rst,cnt);",
                      "initial begin clk=0;"]
        for cycle in range(96):
            reset = "x" if cycle == 20 else "z" if cycle == 50 else str(int(cycle in (0, 37, 38, 58)))
            statements.append(f"rst=1'b{reset}; #1 clk=1; #1 $display(\"{cycle} %0d\",cnt); clk=0;")
        statements.append("end endmodule")
        (work / "tb.sv").write_text("\n".join(statements))
        run(["iverilog", "-g2012", "-s", "tb", "-o", str(work / "ref.vvp"),
             str(source), str(work / "tb.sv")], work)
        reference = run(["vvp", str(work / "ref.vvp")], work).splitlines()
        if len(native) != 96 or native != reference:
            raise RuntimeError(f"Frontend trace mismatch: {source}")
        coverage = json.loads((work / "line.json").read_text())
        if not coverage["bins"] or not coverage["closed"]:
            raise RuntimeError("Missing or incomplete source-mapped line coverage")
        if set(coverage["bins"]) != set(model["source_map"]):
            raise RuntimeError("Line coverage denominator differs from frontend mapping")
        (folder / "trace.txt").write_text("\n".join(native) + "\n")
        (folder / "line.json").write_text(json.dumps(coverage, indent=2))
        return native, {"source_sha256": model["source_sha256"], "samples": len(native),
                        "mapped_lines": [v["line"] for v in model["source_map"].values()],
                        "line_bins": len(coverage["bins"]), "match_icarus": True}


def compare_bitwise_or(folder):
    import itertools,hashlib
    out=folder
    # A failed retry must not leave an earlier PASS report looking current.
    out.mkdir(parents=True,exist_ok=False)
    (out/'dut.sv').write_text('module dut(input logic clk,input logic[1:0] a,b,output logic[1:0]q);always @(posedge clk) q<=a|b;endmodule\n')
    (out/'design.hpp').write_text(cpp_header(Lowerer(out/'dut.sv').lower()))
    sv='module tb;reg clk=0;reg[1:0]a,b;wire[1:0]q;dut d(clk,a,b,q);initial begin '
    cpp='#define MINISIM_NO_DEMO\n#include "minisim.cpp"\n#include "design.hpp"\nGeneratedDesign d;Task stim(){S.set(d.signals.at("clk"),mk(0));co_await Delay{1};'
    def enc(bits):
     v=x=z=0
     for i,c in enumerate(bits[::-1]):
      if c=='1':v|=1<<i
      if c in 'xz':x|=1<<i
      if c=='z':z|=1<<i
     return f'make({v},{x},2,{z})'
    for i,chars in enumerate(itertools.product('01xz',repeat=4)):
     a=''.join(chars[:2]);b=''.join(chars[2:]);sv+=f'a=2\'b{a};b=2\'b{b};#1;clk=1;#1;$display("ROW {i} %b",q);clk=0;#1;'
     cpp+=f'S.set(d.signals.at("a"),{enc(a)});S.set(d.signals.at("b"),{enc(b)});co_await Delay{{1}};S.set(d.signals.at("clk"),mk(1));co_await Delay{{1}};{{auto q=S.get(d.signals.at("q"));std::cout<<"ROW {i} ";for(int bit=1;bit>=0;--bit)std::cout<<((q.x>>bit)&1?\'x\':(q.v>>bit)&1?\'1\':\'0\');std::cout<<"\\n";}}S.set(d.signals.at("clk"),mk(0));co_await Delay{{1}};'
    sv+='$finish;end endmodule';cpp+='S.finished=true;}int main(){d=build_generated_design();for(auto&p:d.programs)S.spawn(exec(&p));S.spawn(stim());S.run(2000);}'
    (out/'tb.sv').write_text(sv);(out/'harness.cpp').write_text(cpp)
    def run(cmd):return subprocess.check_output(cmd,cwd=out,text=True,stderr=subprocess.STDOUT,timeout=120)
    run(['iverilog','-g2012','-s','tb','-o','reference.vvp','dut.sv','tb.sv']);ref=run(['vvp','reference.vvp']);(out/'reference.log').write_text(ref)
    run(['g++','-std=c++20','-O0','-I'+str(ROOT/'src'),'harness.cpp','-o','native']);actual=run(['./native']);(out/'native.log').write_text(actual)
    rows=lambda t:[l for l in t.splitlines() if l.startswith('ROW ')]
    if rows(ref)!=rows(actual) or len(rows(actual))!=256:
        raise RuntimeError("OR native/reference trace mismatch or incomplete samples")
    # A logical-OR substitution must disagree on multi-bit results.
    p=out/'design.hpp';clean=p.read_text()
    if 'Op::Or' not in clean:raise RuntimeError('Missing intended OR mutation site')
    try:
        p.write_text(clean.replace('Op::Or','Op::LogOr'))
        run(['g++','-std=c++20','-O0','-I'+str(ROOT/'src'),'harness.cpp','-o','mutant'])
        bad=run(['./mutant']);(out/'mutant.log').write_text(bad)
    finally:
        p.write_text(clean)
    if len(rows(bad))!=256:raise RuntimeError('Incomplete OR mutant trace')
    mismatches=sum(a!=b for a,b in zip(rows(ref),rows(bad)))
    if not mismatches:raise RuntimeError('Logical OR mutant escaped oracle')
    result={'four_state_pairs':256,'icarus_equal':True,'logical_or_mutant_mismatches':mismatches,'scope':'flat unsigned two-bit bitwise OR through pyslang IR and native scheduler; not full language support','rtl_sha256':hashlib.sha256((out/'dut.sv').read_bytes()).hexdigest()};(out/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("build/coverage/frontend"))
    parser.add_argument("--bitwise-or-only", action="store_true",
                        help="Run exhaustive four-state OR and logical-OR mutant only")
    args = parser.parse_args()
    output = args.output.resolve()
    if args.bitwise_or_only:
        compare_bitwise_or(output)
        return
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "examples/counter.sv"
    text = source.read_text()
    summary = {"schema": "minisim-frontend-validation-v1", "cases": {}}
    traces = {}
    for name, content in (("clean", text), ("width8", text.replace("[3:0]", "[7:0]")),
                          ("increment2", text.replace("cnt + 1", "cnt + 2"))):
        # Keep exact clean source bytes and stable variant paths for provenance.
        target = output / name
        target.mkdir(exist_ok=True)
        if name == "clean":
            (target / "input.sv").write_bytes(source.read_bytes())
        else:
            (target / "input.sv").write_text(content)
        rtl = source if name == "clean" else target / "input.sv"
        traces[name], summary["cases"][name] = compare(rtl, target)
        print(f"{name}: 96 samples match Icarus; source-mapped line bins hit")
    if traces["increment2"] == traces["clean"] or traces["width8"] == traces["clean"]:
        raise RuntimeError("RTL variants did not alter execution; possible hard-coded model")
    summary["rtl_changes_affect_execution"] = True
    (output / "summary.json").write_text(json.dumps(summary, indent=2))
    print(f"Frontend coverage artifacts: {output}")


if __name__ == "__main__":
    main()
