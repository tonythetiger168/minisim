"""Compare the C++ value library with real Icarus; requires g++, iverilog, vvp.

Run: python3 tests/compare_mux.py
No slang frontend or full Minisim RTL execution is involved.
"""
from pathlib import Path
import itertools
import subprocess
import tempfile


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True,
                          timeout=120).stdout


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="minisim_mux_") as folder:
        tmp = Path(folder)
        exe = tmp / "mux"
        run("g++", "-std=c++20", "-O2", "-I", str(root / "src"),
            str(root / "tests/test_mux.cpp"), "-o", str(exe))
        # Capture even a failing unit-test executable to show differential evidence.
        cpp = subprocess.run([str(exe), "--dump"], capture_output=True,
                             text=True, timeout=120)
        vectors = ["".join(v) for v in itertools.product("01xz", repeat=2)]
        statements = [f'$display("%b", (1\'b{c} ? 2\'b{a} : 2\'b{b}));'
                      for c, a, b in itertools.product("01xz", vectors, vectors)]
        source = tmp / "mux.sv"
        source.write_text("module tb; initial begin\n" + "\n".join(statements)
                          + "\nend endmodule\n", encoding="utf-8")
        run("iverilog", "-g2012", "-s", "tb", "-o", str(tmp / "mux.vvp"), str(source))
        reference = run("vvp", str(tmp / "mux.vvp")).splitlines()
        actual = cpp.stdout.splitlines()
        if len(reference) != 1024 or len(actual) != 1024:
            raise SystemExit("FAIL: incomplete output")
        mismatches = sum(a != b for a, b in zip(actual, reference))
        print(f"Icarus differential: {len(reference)} cases, {mismatches} mismatches")
        if mismatches or cpp.returncode:
            print(cpp.stderr, end="")
            raise SystemExit(1)


if __name__ == "__main__":
    main()
