// IEEE 1800-2023 14.9, 14.10, 16.16: a clocking block in an interface,
// driven and sampled through hierarchical names, and used as the clock of an
// assertion (@(b.cb) is its clocking event).
//! icarus-skip: Icarus 13 does not support clocking blocks
interface bus_if(input logic clk);
  logic req, gnt;
  clocking cb @(posedge clk);
    output req;
    input gnt;
  endclocking
endinterface
module dut(input logic clk, input logic req, output logic gnt);
  always @(posedge clk) gnt <= req;
endmodule
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  bus_if b(clk);
  dut u(clk, b.req, b.gnt);
  a1: assert property (@(b.cb) b.req |=> b.gnt) $display("%0t a1 pass", $time); else $display("%0t a1 fail", $time);
  initial begin
    b.req = 0;
    @(b.cb) b.cb.req <= 1;
    @(b.cb) $display("%0t gnt=%b", $time, b.cb.gnt);
    b.cb.req <= 0;
    @(b.cb) $display("%0t gnt=%b", $time, b.cb.gnt);
    @(b.cb) $display("%0t gnt=%b", $time, b.cb.gnt);
    #1 $finish;
  end
endmodule

//> 5 a1 pass
//> 15 gnt=0
//> 25 gnt=1
//> 25 a1 pass
//> 25 a1 pass
//> 35 gnt=0
//> 35 a1 pass
