// Instances of one module outside the cycle domain (msim -no-cycle, or a
// design that is nearly all comb logic, such as OpenTitan): each comb and
// clocked process is a function of its own, and msim -compile emits one
// copy per module that every instance calls with its own signal distances
// (codegen.hpp shareEv). Ports bound to the parent's signals move by
// another distance than the instance's own; a parameter in an expression
// keeps that instance's own copy. Every instance must compute its own values.
module evcell #(parameter int K = 3) (input logic clk, input logic rst, input logic [7:0] a, input logic [7:0] b,
                                      output logic [7:0] y, output logic [7:0] q);
  logic [7:0] s, t, u;
  assign s = a + b;
  assign t = s ^ {s[3:0], s[7:4]};
  assign u = t + 8'(K);
  always_comb begin
    if (t[0]) y = u;
    else y = ~u;
  end
  always_ff @(posedge clk or posedge rst)
    if (rst) q <= 8'h00;
    else if (y[1]) q <= q + y;
    else q <= q ^ y;
endmodule

module shareev;
  logic clk = 0, rst = 0;
  logic [7:0] a0 = 8'h11, a1 = 8'h22, a2 = 8'h33, a3 = 8'h44;
  logic [7:0] y0, y1, y2, y3, q0, q1, q2, q3;
  always #5 clk = ~clk;
  evcell #(3) c0 (clk, rst, a0, q3, y0, q0);
  evcell #(3) c1 (clk, rst, a1, q0, y1, q1);
  evcell #(3) c2 (clk, rst, a2, q1, y2, q2);
  evcell #(7) c3 (clk, rst, a3, q2, y3, q3);
  always @(posedge clk) begin
    a0 <= a0 + 8'd1;
    a2 <= a2 ^ q1;
  end
  initial begin
    #1 rst = 1;
    #11 rst = 0;
    repeat (20) @(negedge clk) $display("%0t y=%h %h %h %h q=%h %h %h %h", $time, y0, y1, y2, y3, q0, q1, q2, q3);
    $finish;
  end
endmodule
