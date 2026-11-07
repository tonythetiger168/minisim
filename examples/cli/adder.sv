// DUT for the msim command-line test: a registered adder whose width comes
// from a parameter, so +define+WIDTH=... in the testbench reaches it.
module adder #(parameter int W = 4) (
  input  logic         clk,
  input  logic [W-1:0] a, b,
  output logic [W-1:0] sum
);
  always_ff @(posedge clk) sum <= a + b;
endmodule
