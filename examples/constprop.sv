// -compile folds signals that only ever hold one constant (msim.cpp
// propagateConstants): tied-off inputs, a parameter-driven select, a
// chain of port copies, a declaration's initializer. Not a signal an NBA
// writes. Against the interpreter.
module constprop_core #(parameter int MODE = 2) (
    input logic clk, input logic en_i, input logic [3:0] sel_i, input logic [7:0] d_i,
    output logic [7:0] q_o, output logic [7:0] r_o);
  logic [7:0] acc = 0, acc2 = 0;
  logic [1:0] mode;
  logic       en;
  logic [7:0] bias = 8'h11;           // never written again
  assign mode = MODE[1:0];
  assign en = en_i & (mode == 2'd2);  // folds to en_i
  always_comb begin
    r_o = d_i;
    if (sel_i == 4'd0) r_o = d_i + bias;
    else if (sel_i == 4'd3) r_o = d_i ^ 8'hff;
  end
  always_ff @(posedge clk) begin
    if (en) acc <= acc + r_o;
    acc2 <= acc2 + {4'd0, sel_i} + bias;
  end
  assign q_o = acc ^ acc2;
endmodule
module constprop;
  logic clk = 0;
  logic [7:0] d = 1;
  wire  [7:0] q, r, q2, r2;
  wire        tie1 = 1'b1;
  wire        tie0;
  assign tie0 = 1'b0;
  logic [3:0] sel3;
  assign sel3 = 4'd3;
  constprop_core u0 (.clk, .en_i(tie1), .sel_i(4'd0), .d_i(d), .q_o(q), .r_o(r));
  constprop_core #(.MODE(1)) u1 (.clk, .en_i(tie1), .sel_i(sel3), .d_i(d), .q_o(q2), .r_o(r2));
  always #5 clk = ~clk;
  always @(posedge clk) d <= d + 8'd7;
  initial begin
    repeat (6) @(negedge clk) $display("q=%h r=%h q2=%h r2=%h tie0=%b", q, r, q2, r2, tie0);
    $finish;
  end
endmodule
