"""Adaptive stimulus -> pyslang-lowered Minisim DUT -> replay on Icarus/Verilator.

Reference agreement is independent of the +1 golden scoreboard. Wrong RTL must
agree with its own reference simulation while still failing golden verification.
"""
import argparse
import csv
import hashlib
import json
from importlib.metadata import version
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from pyslang_lower import Lowerer, cpp_header


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, capture_output=True, text=True,
                          check=True, timeout=120).stdout


def replay_icarus(source, width, rows, work):
    lines = [f'module tb; reg clk,rst; wire [{width-1}:0] cnt; counter dut(clk,rst,cnt);',
             'initial begin clk=0;']
    for cycle, reset, _ in rows:
        lines.append(f'rst={reset}; #1 clk=1; #1 $display("{cycle},{reset},%0d",cnt); clk=0;')
    lines.append('end endmodule')
    (work / "replay.sv").write_text("\n".join(lines))
    run(["iverilog", "-g2012", "-s", "tb", "-o", str(work / "ref.vvp"),
         str(source), str(work / "replay.sv")], work)
    return run(["vvp", str(work / "ref.vvp")], work).splitlines()


def replay_verilator(source, rows, work, target):
    resets = ",".join(reset for _, reset, _ in rows)
    harness = work / "replay.cpp"
    harness.write_text('''#include "Vcounter.h"
#include "verilated.h"
#include "verilated_cov.h"
#include <iostream>
int main() {
  VerilatedContext context; Vcounter dut(&context);
  int resets[] = {''' + resets + '''};
  for(unsigned n=0; n<sizeof(resets)/sizeof(resets[0]); ++n) {
    dut.clk=0; dut.rst=resets[n]; dut.eval(); context.timeInc(1);
    dut.clk=1; dut.eval(); context.timeInc(1);
    std::cout << n << ',' << resets[n] << ',' << (unsigned)dut.cnt << '\\n';
  }
  dut.final(); context.coveragep()->write("coverage.dat");
}
''')
    result = subprocess.run(["verilator", "--cc", "--exe", "--build", "--coverage", "-j", "2",
                             "--top-module", "counter", "-Mdir", str(work / "obj"),
                             str(source), str(harness)], cwd=work, capture_output=True,
                            text=True, timeout=180)
    (target / "verilator-build.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    reference = run([str(work / "obj/Vcounter")], work).splitlines()
    shutil.copyfile(work / "coverage.dat", target / "verilator.dat")
    return reference


def verify_case(source, target, expected_failure, budget=512, hold_reset=False):
    target.mkdir(parents=True, exist_ok=True)
    model = Lowerer(source).lower()
    (target / "design.json").write_text(json.dumps(model, indent=2))
    (target / "design.hpp").write_text(cpp_header(model))
    with tempfile.TemporaryDirectory(prefix="minisim_adaptive_") as temporary:
        work = Path(temporary)
        exe = work / "native"
        run(["g++", "-std=c++20", "-O2", "-I", str(ROOT / "src"), "-I", str(target),
             "-I", str(ROOT / "tests/support"), str(ROOT / "tests/frontend_coverage_harness.cpp"),
             "-o", str(exe)], work)
        command = [str(exe), "--budget", str(budget)] + (["--hold-reset"] if hold_reset else [])
        result = subprocess.run(command, cwd=work, capture_output=True, text=True, timeout=15)
        (target / "native.log").write_text(result.stdout + result.stderr)
        coverage = json.loads((work / "coverage.json").read_text())
        if coverage["failure"] != expected_failure:
            raise RuntimeError(f'Unexpected outcome: {source}: {coverage["failure"]}')
        expected_code = 10 if expected_failure == "scoreboard_mismatch" else 11 if expected_failure else 0
        if result.returncode != expected_code or coverage["passed"] != (not expected_failure):
            raise RuntimeError("Exit status and verification report disagree")
        if not expected_failure:
            for group in ("functional", "line", "toggle", "fsm_states", "fsm_arcs"):
                if not coverage[group]["bins"] or not coverage[group]["closed"]:
                    raise RuntimeError(f"Vacuous or incomplete coverage: {group}")
        if set(coverage["line"]["bins"]) != set(model["source_map"]):
            raise RuntimeError("Line coverage denominator differs from AST mapping")
        rows = list(csv.reader((work / "stimulus.csv").read_text().splitlines()))
        if not rows or len(rows) != coverage["samples"]:
            raise RuntimeError("Missing stimulus or mismatched sample count")
        if expected_failure == "scoreboard_mismatch":
            if coverage["properties"]["correct_failed"] != 1:
                raise RuntimeError("Mismatch was not recorded as a property failure")
            if coverage["functional"]["bins"].get("count:" + rows[-1][2], 0) != 0:
                raise RuntimeError("Incorrect sample was credited toward coverage")
        if expected_failure == "cycle_budget_exhausted":
            if coverage["functional"]["closed"] or coverage["properties"]["wrap_passed"]:
                raise RuntimeError("Stalled run falsely claimed functional closure")
        native = [",".join(row) for row in rows]
        reference = replay_icarus(source, model["signals"]["cnt"], rows, work)
        if native != reference:
            raise RuntimeError("Icarus disagrees with generated Minisim DUT")
        verilator = replay_verilator(source, rows, work, target)
        if native != verilator:
            raise RuntimeError("Verilator disagrees with generated Minisim DUT")
        for file in ("coverage.json", "stimulus.csv"):
            shutil.copyfile(work / file, target / file)
        (target / "icarus.trace").write_text("\n".join(reference) + "\n")
        (target / "verilator.trace").write_text("\n".join(verilator) + "\n")
    return {"samples": len(rows), "expected_failure": expected_failure,
            "verification_passed": coverage["passed"], "matches_icarus": True,
            "matches_verilator": True, "source_sha256": model["source_sha256"],
            "stimulus_sha256": hashlib.sha256((target / "stimulus.csv").read_bytes()).hexdigest(),
            "generated_header_sha256": hashlib.sha256((target / "design.hpp").read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("build/coverage/adaptive"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "examples/counter.sv"
    text = source.read_text()
    cases = (("clean4", text, "", 512, False),
             ("clean8", text.replace("[3:0]", "[7:0]"), "", 512, False),
             ("mutant2", text.replace("cnt + 1", "cnt + 2"), "scoreboard_mismatch", 512, False),
             ("stalled", text, "cycle_budget_exhausted", 8, True))
    files = ("src/minisim.cpp", "src/prog.hpp", "src/expr.hpp", "src/exec.inc",
             "src/coverage.hpp", "tools/pyslang_lower.py", "tests/support/counter_model.hpp",
             "tests/frontend_coverage_harness.cpp", "tests/run_coverage_driven_frontend.py")
    summary = {"schema": "minisim-adaptive-verification-v1", "cases": {},
               "provenance": {"pyslang": version("pyslang"),
                              "python": sys.version,
                              "git_head": run(["git", "rev-parse", "HEAD"], ROOT).strip(),
                              "tools": {name: run(command, ROOT).splitlines()[0]
                                        for name, command in (("g++", ["g++", "--version"]),
                                                              ("iverilog", ["iverilog", "-V"]),
                                                              ("verilator", ["verilator", "--version"]))},
                              "source_sha256": {file: hashlib.sha256((ROOT / file).read_bytes()).hexdigest()
                                                for file in files}}}
    for name, content, failure, budget, hold_reset in cases:
        target = output / name
        target.mkdir(exist_ok=True)
        rtl = target / "input.sv"
        rtl.write_text(content)
        summary["cases"][name] = verify_case(rtl, target, failure, budget, hold_reset)
        print(f'{name}: {summary["cases"][name]["samples"]} samples; '
              f'{failure or "CLOSED"}; Icarus and Verilator agree')
    (output / "summary.json").write_text(json.dumps(summary, indent=2))
    print(f"Adaptive verification evidence: {output}")


if __name__ == "__main__":
    main()
