"""Reference RTL line/toggle/SVA and explicit FSM cover-property checks (Linux/WSL).

Native automatic FSM extraction is not assumed on Verilator 5.032.
"""
import argparse
import json
from pathlib import Path
import re
import resource
import shutil
import subprocess
import tempfile


def no_core():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run(command, cwd, log):
    result = subprocess.run(command, cwd=cwd, text=True, capture_output=True,
                            timeout=180, preexec_fn=no_core)
    log.write_text(result.stdout + result.stderr)
    return result


def coverage_points(path):
    points = []
    for line in path.read_text().splitlines():
        match = re.fullmatch(r"C '(.*)' (\d+)", line)
        if match:
            fields = dict(field.split("\x02", 1) for field in
                          match[1].split("\x01") if "\x02" in field)
            points.append({"fields": fields, "hits": int(match[2])})
    if not points:
        raise RuntimeError("Empty/unrecognized Verilator coverage output")
    return points


def property_hits(points, names):
    return {name: sum(p["hits"] for p in points if name in p["fields"].values())
            for name in names}


def validate_end_case(text, returncode, mode, mutant=False):
    """Validate actual sampled obligations, not just an exit code or marker."""
    if mode not in {"drained", "undrained", "no-start", "reset"}:
        raise RuntimeError("Unknown end-case")
    expected_cycles = 7 if mode == "undrained" else 8
    samples = re.findall(r"^SAMPLE cycle=(\d+) state=(\d+) cnt=(\d+) pending=([01])$", text, re.M)
    if [int(x[0]) for x in samples] != list(range(expected_cycles)):
        raise RuntimeError("Missing/duplicate/out-of-order obligation samples")
    if mutant and mode == "drained":
        if returncode == 0 or "ap_run_done" not in text or "OBLIGATION" in text:
            raise RuntimeError("Drained mutant not rejected by intended RTL assertion")
        return {"rtl_assertion_rejected": "ap_run_done"}
    expected = {"drained": (0, (1, 1, 0, 0, 0)),
                "undrained": (2, (1, 0, 0, 1, 0)),
                "no-start": (3, (0, 0, 0, 0, 0)),
                "reset": (3, (1, 0, 1, 0, 0))}[mode]
    summaries = re.findall(r"^OBLIGATION triggers=(\d+) resolved=(\d+) cancelled=(\d+) pending=([01]) failures=(\d+)$", text, re.M)
    if returncode != expected[0] or len(summaries) != 1 or tuple(map(int, summaries[0])) != expected[1]:
        raise RuntimeError("Unexpected obligation counts or exit status")
    if "ap_run_done" in text:
        raise RuntimeError("Unexpected assertion failure")
    return dict(zip(("triggers", "resolved", "cancelled", "pending", "failures"), map(int, summaries[0])))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=Path("build/coverage/sva"))
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[1]
    version = subprocess.check_output(["verilator", "--version"], text=True).strip()
    with tempfile.TemporaryDirectory(prefix="minisim_vlt_coverage_") as folder:
        temp = Path(folder)
        for label, bug in (("clean", 0), ("mutant", 1)):
            work = temp / label
            work.mkdir()
            result = run(["verilator", "--cc", "--exe", "--build", "-j", "2",
                          "--assert", "--coverage", "--top-module", "coverage_fsm",
                          f"-GBUG=1'b{bug}", "-Mdir", str(work / "obj"),
                          str(root / "tests/rtl/coverage_fsm.sv"),
                          str(root / "tests/rtl/coverage_harness.cpp")], work,
                         output / f"{label}-build.log")
            if result.returncode:
                raise RuntimeError(f"{label} build failed; see {output}")
            # Dedicated end-of-run schedules use the same compiled fixture.
            for mode, expected in (("drained", 0), ("undrained", 2),
                                   ("no-start", 3), ("reset", 3)):
                end_result = run([str(work / "obj/Vcoverage_fsm"), "--end-case", mode],
                                 work, output / f"{label}-end-{mode}.log")
                text = end_result.stdout + end_result.stderr
                validate_end_case(text, end_result.returncode, mode, mutant=bool(bug))
            result = run([str(work / "obj/Vcoverage_fsm")], work,
                         output / f"{label}-run.log")
            if bug:
                if result.returncode == 0 or "ap_run_done" not in result.stdout + result.stderr:
                    raise RuntimeError("Mutant was not rejected by ap_run_done")
                summary["mutant_assertion_detected"] = "ap_run_done"
                (output / "summary.json").write_text(json.dumps(summary, indent=2))
                print("SVA MUTANT DETECTED: ap_run_done rejected incorrect FSM transition")
                continue
            if result.returncode:
                raise RuntimeError(f"Clean SVA run failed; see {output}")
            saved = output / "clean.dat"
            shutil.copyfile(work / "coverage.dat", saved)
            points = coverage_points(saved)
            required = ("cp_state_idle", "cp_state_run", "cp_state_done",
                        "cp_idle_run", "cp_run_done", "cp_done_idle")
            hits = property_hits(points, required)
            if not all(hits.values()):
                raise RuntimeError(f"FSM/functional SVA coverage holes: {hits}")
            pages = sorted({p["fields"].get("page", "") for p in points})
            if not any(p.startswith("v_line") for p in pages) or not any(
                    p.startswith("v_toggle") for p in pages):
                raise RuntimeError("Line or toggle instrumentation missing")
            subprocess.run(["verilator_coverage", "--write-info", str(output / "clean.info"),
                            str(saved)], check=True, timeout=30)
            summary = {"verilator": version, "cycles": 80,
                       "fsm_method": "explicit cover property states/arcs",
                       "clean_assertions_no_failures": True,
                       "cover_property_hits": hits, "raw_points": points,
                       "categories": {page: {
                           "total": sum(p["fields"].get("page") == page for p in points),
                           "hit": sum(p["fields"].get("page") == page and p["hits"] > 0
                                      for p in points)} for page in pages}}
            print(f"CLEAN PASS: 6/6 FSM state/arc cover properties hit; line/toggle collected")
            result = run([str(work / "obj/Vcoverage_fsm"), "--no-start"], work,
                         output / "no-start-run.log")
            if result.returncode:
                raise RuntimeError("No-start reference failed unexpectedly")
            no_start = output / "no-start.dat"
            shutil.copyfile(work / "coverage.dat", no_start)
            no_start_hits = property_hits(coverage_points(no_start), required)
            if no_start_hits["cp_state_idle"] == 0 or any(
                    value for name, value in no_start_hits.items() if name != "cp_state_idle"):
                raise RuntimeError(f"Vacuous/unexpected coverage counted: {no_start_hits}")
            summary["no_start_property_hits"] = no_start_hits
            (output / "summary.json").write_text(json.dumps(summary, indent=2))
            print("NO-START PASS: unvisited states/arcs have zero cover hits")
    print(f"Coverage artifacts: {output}")


if __name__ == "__main__":
    main()
