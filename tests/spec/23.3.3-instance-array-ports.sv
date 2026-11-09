// IEEE 1800-2023 23.3.3.5: port connections of an instance array. An actual
// as wide as the port goes to every instance; one N times as wide is split,
// the left-most instance taking the most significant bits; an unpacked
// array gives each instance one element, left-most to left-most. Each
// element is a named instance (u[2]) with its own parameters and state.
module cnt #(parameter W = 4, parameter START = 0) (input logic clk, output logic [W-1:0] q);
  initial q = START;
  always @(posedge clk) q <= q + 1;
endmodule
module pass(input logic [1:0] a, output logic [1:0] y);
  assign y = a;
  function string where(); return $sformatf("%m"); endfunction
endmodule
module top;
  logic clk = 0;
  logic [1:0] q [0:2];
  cnt #(2, 1) c[2:0] (.clk(clk), .q(q));          // clk to all three; one element of q each
  logic [5:0] a = 6'b11_01_00;
  logic [5:0] y;
  pass p[0:2] (.a(a), .y(y));                      // p[0] gets a[5:4]
  logic [1:0] ua [3];
  logic [1:0] uy [3];
  pass r[2:0] (.a(ua), .y(uy));                    // r[2] gets ua[0]
  initial begin
    ua[0] = 2'd1; ua[1] = 2'd2; ua[2] = 2'd3;
    #2 $display("y=%b uy=%0d %0d %0d", y, uy[0], uy[1], uy[2]);
    $display("%b %b %b %b %b %b", p[0].a, p[1].a, p[2].a, r[0].a, r[1].a, r[2].a);
    $display("%s %s", p[0].where(), r[2].where());
    repeat (2) begin #5 clk = 1; #5 clk = 0; end
    $display("q=%0d %0d %0d c[1].q=%0d", q[0], q[1], q[2], c[1].q);
  end
endmodule
//> y=110100 uy=1 2 3
//> 11 01 00 11 10 01
//> top.p[0].where top.r[2].where
//> q=3 3 3 c[1].q=3
