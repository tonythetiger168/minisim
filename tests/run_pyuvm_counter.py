"""Run the real RTL counter; --check-mutant also requires a known bug to fail."""
import argparse
from pathlib import Path
import tempfile
import sys
import shutil
import subprocess
import xml.etree.ElementTree as ET

from cocotb_tools.runner import get_runner


def simulate(source, folder, root, expect_failure=False, simulator="icarus", coverage_dir=None):
    # cocotb's runner exports the parent interpreter's sys.path to the simulator.
    module_dir = str(root / "tests/pyuvm")
    if module_dir not in sys.path:
        sys.path.insert(0, module_dir)
    runner = get_runner(simulator)
    runner.build(sources=[source], hdl_toplevel="counter", build_dir=folder,
                 always=True, timescale=("1ns", "1ps"),
                 build_args=["--coverage"] if coverage_dir else [])
    xml_path = folder / "results.xml"
    try:
        runner.test(hdl_toplevel="counter", test_module="counter_test",
                    test_dir=folder, results_xml=str(xml_path), seed=168)
    except SystemExit:
        # The XML below must prove this was the intended scoreboard failure.
        if not expect_failure:
            raise
    tree = ET.parse(xml_path)
    cases = tree.findall(".//testcase")
    failures = tree.findall(".//failure")
    errors = tree.findall(".//error")
    if len(cases) != 1 or errors or tree.findall(".//skipped"):
        raise RuntimeError("Missing, skipped or invalid simulator result")
    if expect_failure:
        evidence = " ".join(ET.tostring(f, encoding="unicode") for f in failures)
        if not failures or "counter mismatch" not in evidence:
            raise RuntimeError("Mutant did not produce the expected scoreboard mismatch")
        print("MUTANT DETECTED: increment-by-two rejected by scoreboard")
    elif failures:
        raise RuntimeError("Clean counter failed scoreboard")
    else:
        print("CLEAN PASS: 96 cycles checked by pyuvm scoreboard")
    if coverage_dir:
        raw = folder / "coverage.dat"
        if not raw.is_file() or raw.stat().st_size == 0:
            raise RuntimeError("Verilator did not write coverage.dat")
        coverage_dir.mkdir(parents=True, exist_ok=True)
        label = "mutant" if expect_failure else "clean"
        saved = coverage_dir / f"{label}.dat"
        shutil.copyfile(raw, saved)
        subprocess.run(["verilator_coverage", "--write-info",
                        str(coverage_dir / f"{label}.info"), str(saved)], check=True,
                       timeout=30)
        print(f"VERILATOR COVERAGE: {saved}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check-mutant", action="store_true")
    parser.add_argument("--sim", choices=("icarus", "verilator"), default="icarus")
    parser.add_argument("--coverage-dir", type=Path,
                        help="Save Verilator raw coverage and LCOV (keeps clean/mutant separate)")
    args = parser.parse_args()
    if args.coverage_dir and args.sim != "verilator":
        parser.error("--coverage-dir requires --sim verilator")
    coverage_dir = args.coverage_dir.resolve() if args.coverage_dir else None
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="minisim_pyuvm_") as temp:
        folder = Path(temp)
        source = root / "examples/counter.sv"
        simulate(source, folder / "clean", root, simulator=args.sim, coverage_dir=coverage_dir)
        if args.check_mutant:
            text = source.read_text()
            if text.count("cnt + 1") != 1:
                raise RuntimeError("Counter fixture changed; cannot inject known mutant")
            mutant = folder / "counter_mutant.sv"
            mutant.write_text(text.replace("cnt + 1", "cnt + 2"))
            simulate(mutant, folder / "mutant", root, expect_failure=True,
                     simulator=args.sim, coverage_dir=coverage_dir)


if __name__ == "__main__":
    main()
