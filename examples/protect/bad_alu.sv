// A protected block with a mistake in it: msim must report the error
// without showing the protected text (no names, no source line).
module secret_alu(input logic clk, input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
`pragma protect begin
  always @(posedge clk) y <= secret_typo + a;
`pragma protect end
endmodule
