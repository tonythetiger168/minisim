// An IP block whose internals are protected (IEEE 1800-2023 clause 34).
// The ports stay in the clear; everything between `pragma protect begin and
// `pragma protect end is encrypted by msimprotect:
//
//   msimprotect -key vendor_pub.pem -keyowner "Acme" -keyname acme-1 secret_alu.sv
//   msim -keyfile vendor_priv.pem secret_alu.svp alu_tb.sv
module secret_alu(input logic clk, input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
`pragma protect begin
  logic [7:0] secret_acc;
  initial secret_acc = 8'h5a;
  always @(posedge clk) begin
    secret_acc <= secret_acc + (a ^ b);
    y <= secret_acc;
  end
`pragma protect end
endmodule
