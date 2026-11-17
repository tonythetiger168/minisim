// An ALU that uses a package from another file and a macro from a header.
module alu
  import alu_pkg::*;
#(parameter int W = `ALU_W)
(
  input  logic         clk,
  input  op_t          op,
  input  logic [W-1:0] a, b,
  output logic [W-1:0] y
);
  logic [W-1:0] r;  // 結果 (UTF-8 comment before code on a line)
  always_comb r = apply(op, a, b);
  wire /*寬度 W*/ [W-1:0] w2 = r;
  always_ff @(posedge clk) y <= r;
endmodule
