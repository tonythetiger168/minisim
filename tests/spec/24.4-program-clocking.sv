// IEEE 1800-2023 24.4, 14: a program reads the design through a clocking
// block and drives it with synchronous drives.
//! icarus-skip: Icarus 13 runs a program in the active region, like a module
module dut(input logic clk, input logic [3:0] d, output logic [3:0] q);
  always @(posedge clk) q <= d + 1;
endmodule

module top;
  logic clk = 0;
  logic [3:0] d = 0, q;
  always #5 clk = ~clk;
  dut u(clk, d, q);
  tb t(clk, d, q);
endmodule

program tb(input logic clk, output logic [3:0] d, input logic [3:0] q);
  clocking cb @(posedge clk);
    input q;
    output d;
  endclocking
  // A drive at a clocking event takes effect in that time step, after the
  // design has read d (14.16); cb.q is the value of q just before the edge.
  initial begin
    @(cb);
    cb.d <= 3;
    @(cb);
    @(cb);
    $display("%0t q=%0d", $time, cb.q);
    cb.d <= 7;
    @(cb);
    @(cb);
    $display("%0t q=%0d", $time, cb.q);
  end
endprogram

//> 25 q=4
//> 45 q=8
