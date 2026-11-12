"""Run Minisim's native coverage-driven IR fixture and retain its JSON report."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("build/coverage/native"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[1]
    report = output / "coverage.json"
    with tempfile.TemporaryDirectory(prefix="minisim_native_coverage_") as folder:
        executable = Path(folder) / "coverage"
        subprocess.run(["g++", "-std=c++20", "-O2", "-I", str(root / "src"),
                        str(root / "tests/test_coverage.cpp"), "-o", str(executable)],
                       check=True, timeout=120)
        subprocess.run([str(executable), "--report", str(report)], cwd=output,
                       check=True, timeout=10)
    data = json.loads(report.read_text())
    for group, expected in (("line", 2), ("toggle", 12), ("fsm_states", 2),
                            ("fsm_arcs", 4), ("functional", 20)):
        bins = data[group]["bins"]
        if len(bins) != expected or not all(bins.values()) or not data[group]["closed"]:
            raise RuntimeError(f"Incomplete coverage: {group}")
        print(f"{group}: {len(bins)}/{expected} bins hit")
    properties = data["properties"]
    if (properties["count_correct_failed"] or properties["wrap_failed"] or
            properties["wrap_pending"] or properties["wrap_passed"] == 0):
        raise RuntimeError("Property failure, pending obligation or vacuous cover")
    print(f"Native coverage report: {report}")


if __name__ == "__main__":
    main()
