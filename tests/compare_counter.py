"""Compare hand-built Minisim counter events with examples/counter.sv in Icarus.

This validates the demo scheduler/IR path, not the uncompiled slang lowerer.
"""
from pathlib import Path
import subprocess
import tempfile


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True,
                          check=True, timeout=120).stdout


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="minisim_counter_") as folder:
        tmp = Path(folder)
        exe = tmp / "minisim"
        run(["g++", "-std=c++20", "-O2", "-I", str(root / "src"),
             str(root / "src/minisim.cpp"), "-o", str(exe)], tmp)
        actual = run([str(exe)], tmp)
        (tmp / "tb.sv").write_text('''module tb;
reg clk, rst;
wire [3:0] cnt;
counter dut(clk, rst, cnt);
always #5 clk = ~clk;
always @(cnt) $display("t=%0d cnt=%0d", $time, cnt);
initial begin
  clk = 0; rst = 1;
  #22 rst = 0;
  #100 $display("$finish at t=%0d", $time);
  $finish;
end
endmodule
''', encoding="utf-8")
        run(["iverilog", "-g2012", "-s", "tb", "-o", str(tmp / "counter.vvp"),
             str(root / "examples/counter.sv"), str(tmp / "tb.sv")], tmp)
        reference = run(["vvp", str(tmp / "counter.vvp")], tmp)
        def events(text):
            return [line for line in text.splitlines()
                    if line.startswith(("t=", "$finish at t="))]
        if events(actual) != events(reference) or len(events(actual)) != 12:
            raise SystemExit(f"FAIL\nMinisim:\n{actual}\nIcarus:\n{reference}")
        print("Counter differential: 11 value changes and finish at t=122 match Icarus")


if __name__ == "__main__":
    main()
