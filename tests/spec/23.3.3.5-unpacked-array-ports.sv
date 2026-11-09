// IEEE 1800-2023 23.3.3.5: an unpacked array connected to an instance array
// gives each instance one element, the left-most instance the left-most
// element, for inputs and outputs.
//! icarus-skip: Icarus 13 does not connect an unpacked array to an instance array
module cnt #(parameter START = 0) (input logic clk, output logic [1:0] q);
  initial q = START;
  always @(posedge clk) q <= q + 1;
endmodule
module pass(input logic [1:0] a, output logic [1:0] y);
  assign y = a;
endmodule
module top;
  logic clk = 0;
  logic [1:0] q [0:2];
  cnt #(1) c[2:0] (.clk(clk), .q(q));
  logic [1:0] ua [3];
  logic [1:0] uy [3];
  pass r[2:0] (.a(ua), .y(uy));                    // r[2] gets ua[0]
  initial begin
    ua[0] = 2'd1; ua[1] = 2'd2; ua[2] = 2'd3;
    #2 $display("uy=%0d %0d %0d r=%0d %0d %0d", uy[0], uy[1], uy[2], r[0].a, r[1].a, r[2].a);
    repeat (2) begin #5 clk = 1; #5 clk = 0; end
    $display("q=%0d %0d %0d", q[0], q[1], q[2]);
  end
endmodule
//> uy=1 2 3 r=3 2 1
//> q=3 3 3
