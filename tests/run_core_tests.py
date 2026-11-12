"""Build and run core tests without CMake; requires a C++20 compiler (default g++)."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    scenarios = ("nba", "multi_wait", "edges", "finish_active", "finish_nba",
                 "finish_ir", "vcd_initial", "resume_budget", "resume_budget_time", "ir_budget", "ir_budget_wait",
                 "process_exception", "delay_overflow", "delay_boundary")
    failures = []
    with tempfile.TemporaryDirectory(prefix="minisim_core_") as folder:
        tmp = Path(folder)
        for name in ("test_expr", "test_z", "test_mux", "test_coverage", "test_properties", "test_callbacks", "test_kernel"):
            exe = tmp / name
            subprocess.run([args.cxx, "-std=c++20", "-O2", "-UNDEBUG", "-I",
                            str(root / "src"), str(root / f"tests/{name}.cpp"),
                            "-o", str(exe)], check=True, timeout=120)
            cases = scenarios if name == "test_kernel" else (None,)
            for scenario in cases:
                label = f"{name}:{scenario}" if scenario else name
                output = tmp / ("output_" + label.replace(":", "_"))
                output.mkdir()
                result = subprocess.run([str(exe)] + ([scenario] if scenario else []),
                                        cwd=output, text=True, capture_output=True, timeout=10)
                print(f"{label}: {'PASS' if result.returncode == 0 else 'FAIL'}")
                if result.returncode:
                    failures.append(label)
                    print(result.stdout + result.stderr, end="")
    print(f"{6 + len(scenarios)} scenarios, {len(failures)} failures")
    raise SystemExit(bool(failures))


if __name__ == "__main__":
    main()
