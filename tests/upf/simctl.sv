// Simulation controls (IEEE 1801-2024 6.34, 6.58, 6.59): run with
// simctl.upf. All four blocks are in PD_SW, which VDD_SW powers.
//   u_r   a behavioral model with a real output: its real is not corrupted
//   u_c   a counter: its flop is corrupted, its integer is not
//   u_k   assertions: suspended, reset or killed while PD_SW is off
//   u_m   a model whose simstate behavior a library file sets to DISABLE
module rmodel(input logic [3:0] d, output real v);
  always_comb v = d * 0.25;
endmodule
module cnt(input logic clk, input logic rst, output logic [3:0] q, output integer n);
  always_ff @(posedge clk) q <= rst ? 4'd0 : q + 1;
  always_ff @(posedge clk) n <= rst ? 0 : n + 1;
endmodule
module chk(input logic clk, input logic [3:0] q);
  a_known: assert property (@(posedge clk) !$isunknown(q)) else $display("%0t a_known failed", $time);
  a_kill: assert property (@(posedge clk) !$isunknown(q)) else $display("%0t a_kill failed", $time);
  a_expr: assert property (@(posedge clk) !$isunknown(q)) else $display("%0t a_expr failed", $time);
endmodule
module beh(input logic [3:0] d, output logic [3:0] q);
  assign q = ~d;
endmodule
module dut(input logic clk, input logic [3:0] d, input logic quiet, input logic rst, output real v, output logic [3:0] q, output integer n, output logic [3:0] qm);
  rmodel u_r(.d, .v);
  cnt u_c(.clk, .rst, .q, .n);
  chk u_k(.clk, .q);
  beh u_m(.d, .q(qm));
endmodule
module tb;
  import UPF::*;
  logic clk = 0, quiet = 0, rst = 1;
  logic [3:0] d = 4'h6, q, qm;
  real v;
  integer n;
  dut u_dut(.*);
  always #5 clk = ~clk;
  task show(string what);
    $display("%0t %s: v=%0.2f q=%h n=%0d qm=%h", $time, what, v, q, n, qm);
  endtask
  initial begin
    supply_on("VDD", 1.0);
    supply_on("VDD_SW", 1.0);
    supply_on("VSS", 0.0);
    #12 rst = 0;
    show("on");
    supply_off("VDD_SW");
    #20 show("VDD_SW off");
    supply_on("VDD_SW", 1.0);
    #20 show("VDD_SW on, q is x until reset");
    quiet = 1;
    #20 show("quiet");
    rst = 1;
    #10 rst = 0;
    quiet = 0;
    #20 show("reset");
    $finish;
  end
endmodule
