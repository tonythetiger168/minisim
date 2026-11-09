// IEEE 1800-2023 23.3.3.5: port connections of an instance array. An actual
// as wide as the port goes to every instance; one N times as wide is split,
// the left-most instance taking the most significant bits. Each element is
// a named instance (u[2]) with its own parameters and state.
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
  wire [5:0] q;
  cnt #(2, 1) c[2:0] (.clk(clk), .q(q));          // clk to all three; c[2] drives q[5:4]
  logic [5:0] a = 6'b11_01_00;
  wire [5:0] y;
  pass p[0:2] (.a(a), .y(y));                      // p[0] gets a[5:4]
  initial begin
    #2 $display("y=%b", y);
    $display("%b %b %b", p[0].a, p[1].a, p[2].a);
    $display("%s", p[0].where());
    repeat (2) begin #5 clk = 1; #5 clk = 0; end
    $display("q=%b c[1].q=%0d", q, c[1].q);
  end
endmodule
//> y=110100
//> 11 01 00
//> top.p[0].where
//> q=111111 c[1].q=3
