// Named power state transitions and power state groups (IEEE 1801-2024
// 6.7, 6.22, 9.4): run with transitions.upf. Two cores follow `mode`; the
// power intent names the legal transitions of each core, makes one illegal,
// and groups the two cores into a cluster whose legal states are the
// legal combinations.
module core(input logic clk, output logic [3:0] n);
  always_ff @(posedge clk) n <= n + 1;
endmodule
module dut(input logic clk, input logic [1:0] mode0, input logic [1:0] mode1, output logic [3:0] a, output logic [3:0] b);
  core u0(.clk, .n(a));
  core u1(.clk, .n(b));
endmodule
module tb;
  import UPF::*;
  logic clk = 0;
  logic [1:0] mode0 = 0, mode1 = 0;
  logic [3:0] a, b;
  dut u_dut(.*);
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VSS", 0.0);
    #1 mode0 = 2;            // OFF -> RUN: turn_on
    #1 mode0 = 1;            // RUN -> SLEEP: suspend
    #1 mode0 = 2;            // SLEEP -> RUN: resume
    #1 mode1 = 2;            // the cluster: ONE -> BOTH
    #1 mode0 = 0;            // RUN -> OFF: turn_off; cluster BOTH -> ONE
    #1 mode0 = 1;            // OFF -> SLEEP: the illegal transition
    #1 mode0 = 3;            // no state at all, then RUN: SLEEP -> RUN through UNDEFINED
    #1 mode0 = 2;
    #1 mode1 = 1;            // core 1: RUN -> SLEEP is not named and its transitions are complete
    #1 mode1 = 0; mode0 = 0; // the cluster leaves every legal state
    #1 $finish;
  end
endmodule
