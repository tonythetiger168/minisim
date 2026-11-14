//! args: -top glue_top -enable STY016 -only STY016
// STY016: logic in a module that also instantiates submodules.
module glue_leaf (input logic a, output logic y);
  assign y = ~a;                       // a leaf: logic is its job
endmodule

module glue_wires (input logic [1:0] a, output logic [3:0] y);
  logic [1:0] t;
  glue_leaf u0 (.a(a[0]), .y(t[0]));
  glue_leaf u1 (.a(a[1]), .y(t[1]));
  assign y = {t, 2'b00};               // wiring only: no finding
endmodule

module glue_top (input logic clk, input logic [1:0] a, output logic [3:0] y, output logic z);
  logic [3:0] w;
  glue_wires u_w (.a(a), .y(w));
  assign y = w;
  always_ff @(posedge clk) z <= ^w;    //~ STY016
endmodule
