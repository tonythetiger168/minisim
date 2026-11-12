"""Validate a restricted synthesized VHDL+SV design on the Minisim event kernel.

Requires GHDL, Yosys, Icarus, G++, pyslang. It is NOT native nine-state VHDL or
arbitrary mixed-language testbench co-simulation; the translation is explicit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from netlist_lower import NetlistLowerer, header


def run(args, cwd, log):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True, timeout=120)
    log.write_text(result.stdout + result.stderr)
    result.check_returncode()
    return result.stdout + result.stderr


def traces(text):
    return [(int(n), int(v)) for n,v in re.findall(r"TRACE\s+(\d+)\s+(\d+)", text)]


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("build/coverage/mixed-language"))
    args=parser.parse_args()
    output=args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    versions={name:subprocess.check_output(command,text=True).splitlines()[0] for name,command in
              (("ghdl",["ghdl","--version"]),("yosys",["yosys","-V"]),("g++",["g++","--version"]))}
    vhdl=ROOT/"tests/vhdl/counter.vhd"
    with tempfile.TemporaryDirectory(prefix="minisim_mixed_") as temporary:
        work=Path(temporary)
        tb=work/"reference_tb.vhd"
        tb.write_text('''library ieee;
use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity reference_tb is end;
architecture sim of reference_tb is
  signal clk,rst:std_logic:='0'; signal cnt:std_logic_vector(3 downto 0);
begin
  dut:entity work.vhdl_counter port map(clk,rst,cnt);
  process variable expected:integer:=0;
  begin
    for cycle in 0 to 39 loop
      if cycle=0 or cycle=19 then rst<='1'; expected:=0;
      else rst<='0'; expected:=(expected+1) mod 16; end if;
      wait for 1 ns; clk<='1'; wait for 1 ns;
      assert to_integer(unsigned(cnt))=expected report "golden counter mismatch" severity failure;
      report "TRACE " & integer'image(cycle) & " " & integer'image(to_integer(unsigned(cnt)));
      clk<='0';
    end loop;
    std.env.stop; wait;
  end process;
end;
''')
        run(["ghdl","-a","--std=08",str(vhdl),str(tb)],work,output/"ghdl-analyze.log")
        ref=traces(run(["ghdl","--elab-run","--std=08","reference_tb","--assert-level=error"],
                       work,output/"ghdl-reference.log"))
        if len(ref)!=40: raise RuntimeError("incomplete native VHDL reference")
        # The CLI writes netlist text to stdout, never to an implicit source rewrite.
        generated=subprocess.run(["ghdl","--synth","--std=08","--out=verilog",str(vhdl),"-e","vhdl_counter"],
                                  cwd=work,text=True,capture_output=True,timeout=120,check=True)
        translated=output/"vhdl_counter.v"; translated.write_text(generated.stdout)
        cases={}
        for name,increment in (("clean",1),("mutant",2)):
            target=output/name; target.mkdir(exist_ok=True)
            sv=target/"mixed_top.sv"
            sv.write_text((ROOT/"tests/vhdl/mixed_top.sv").read_text().replace("4'd1",f"4'd{increment}"))
            flattened=target/"flattened.sv"
            script=(f'read_verilog -sv {translated} {sv}; hierarchy -check -top mixed_top; '
                    f'proc; flatten; hierarchy -top mixed_top; clean; write_verilog -noattr {flattened}')
            run(["yosys","-Q","-T","-p",script],work,target/"yosys.log")
            model=NetlistLowerer(flattened).lower()
            (target/"design.json").write_text(json.dumps(model,indent=2))
            (target/"design.hpp").write_text(header(model))
            exe=work/"mixed"
            run(["g++","-std=c++20","-O2","-I",str(ROOT/"src"),"-I",str(target),
                 str(ROOT/"tests/mixed_netlist_harness.cpp"),"-o",str(exe)],work,target/"native-build.log")
            actual=traces(run([str(exe)],work,target/"native.log"))
            expected=[(cycle,(value+1)%16) for cycle,value in ref]
            if len(actual)!=40: raise RuntimeError("incomplete Minisim execution")
            # A separate SV simulator executes the translated VHDL + original SV wrapper.
            statements=['module tb; reg clk,rst; wire[3:0] cnt; mixed_top dut(clk,rst,cnt); initial begin clk=0;']
            for cycle in range(40):
                statements.append(f'rst={int(cycle in (0,19))}; #1 clk=1; #1 $display("TRACE {cycle} %0d",cnt); clk=0;')
            statements.append('end endmodule')
            (work/"replay.sv").write_text("\n".join(statements))
            run(["iverilog","-g2012","-s","tb","-o",str(work/"ref.vvp"),str(translated),str(sv),
                 str(work/"replay.sv")],work,target/"icarus-build.log")
            icarus=traces(run(["vvp",str(work/"ref.vvp")],work,target/"icarus.log"))
            if actual!=icarus: raise RuntimeError("Minisim disagrees with Icarus mixed netlist")
            correct=actual==expected
            if correct!=(name=="clean"): raise RuntimeError("unexpected mixed golden result")
            line=json.loads((work/"line.json").read_text())
            if not line["bins"] or not line["closed"]: raise RuntimeError("incomplete netlist line coverage")
            (target/"line.json").write_text(json.dumps(line,indent=2))
            cases[name]={"samples":len(actual),"matches_icarus":True,"golden_passed":correct,
                         "translated_netlist_line_bins":len(line["bins"])}
            print(f"{name}: 40 samples match Icarus; native-VHDL-derived golden {'PASS' if correct else 'REJECTED'}")
    summary={"scope":"synthesizable VHDL-to-Verilog + SV flattened execution on Minisim",
             "native_vhdl_scheduler":False,"std_logic_nine_state_supported":False,
             "versions":versions,"cases":cases,
             "source_sha256":{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in (vhdl,ROOT/"tests/vhdl/mixed_top.sv",ROOT/"tools/netlist_lower.py")}}
    (output/"summary.json").write_text(json.dumps(summary,indent=2))
    print(f"Mixed synthesized-subset evidence: {output}")


if __name__=="__main__": main()
