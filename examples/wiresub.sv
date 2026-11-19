// Copies of whole signals (a = b): clock and reset buffers, port
// connections, feed-throughs. msim -compile reads b where a is read and
// drops the copy (msim.cpp substituteWires). Readers here: an edge wait
// on a buffered clock, a part select, a function, $display, wait(), a
// final block. A forced copy and a net with a second driver keep their
// assigns, and so does a loop of copies.
module cbuf(input logic i, output logic o);
  assign o = i;
endmodule

module stage #(parameter int W = 8) (input logic clk, input logic [W-1:0] d, output logic [W-1:0] q);
  logic [W-1:0] d_buf;
  assign d_buf = d;
  always_ff @(posedge clk) q <= d_buf + 1;
endmodule

module wiresub;
  logic clk = 0, rst_n = 0;
  logic clk_b1, clk_b2;
  cbuf u_b1(.i(clk), .o(clk_b1));
  cbuf u_b2(.i(clk_b1), .o(clk_b2));
  logic [7:0] q1, q2, q2_copy, fwd;
  logic [7:0] cnt = 0;
  always @(posedge clk) cnt <= cnt + 1;
  stage #(8) u_s1(.clk(clk_b2), .d(cnt), .q(q1));
  stage #(8) u_s2(.clk(clk_b1), .d(q1), .q(q2));
  assign q2_copy = q2;
  assign fwd = q2_copy;
  function automatic logic [3:0] lo4(logic [7:0] v); return v[3:0]; endfunction
  logic [3:0] fl;
  assign fl = lo4(fwd);
  // A forced copy keeps its assign.
  logic [7:0] fc;
  assign fc = q1;
  // A loop of copies with no other driver stays as it is (x).
  wire [1:0] l1, l2;
  assign l1 = l2;
  assign l2 = l1;
  // A net with two drivers keeps both.
  wire [7:0] two;
  assign two = q1;
  assign two = q2;
  int cyc = 0;
  always @(posedge clk_b2) begin
    cyc <= cyc + 1;
    $display("%0t cyc=%0d q1=%0d q2=%0d fwd=%0d fwd[7:4]=%h fl=%h fc=%0d l1=%b two=%b", $time, cyc, q1, q2, fwd, fwd[7:4], fl, fc, l1, two);
  end
  always #5 clk = ~clk;
  initial begin
    wait (fwd == 8'd6);
    $display("%0t fwd reached 6", $time);
    #22 force fc = 8'hee;
    #20 release fc;
    #30 $finish;
  end
  final $display("final fwd=%0d clk_b2=%b", fwd, clk_b2);
endmodule
