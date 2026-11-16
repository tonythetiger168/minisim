// IEEE 1800-2023 9.2.2.4: a variable an always_ff writes shall not be
// written by another process. msim warns (on stderr) and runs the design,
// as other simulators do with an initial reset of a register: the reset
// value holds until the first clock edge. The slices of a variable that
// the instances of an instance array drive are one driver each (23.3.3.5)
// and draw no warning.
module inc(input logic [1:0] a, output logic [1:0] y);
  assign y = a + 2'd1;
endmodule
module top;
  logic clk = 0;
  logic [3:0] q;
  logic [3:0] a = 4'b10_01, y;
  inc u[1:0] (.a(a), .y(y));
  always_ff @(posedge clk) q <= q + 1;
  initial q = 4'd5;
  initial begin
    #1 $display("q=%0d y=%b", q, y);
    clk = 1;
    #1 $display("q=%0d", q);
  end
endmodule
//> q=5 y=1110
//> q=6
