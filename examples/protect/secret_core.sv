// A whole module protected with an IEEE 1735 version 3 envelope. Its rights
// (given with msimprotect -control, or a begin_toolblock recipe) decide what
// msim, mlint and mdft may show of it; the viewport pragma makes `stage`
// visible whatever the rights say (IEEE 1735 9.5).
//
//   msimprotect -key vendor_pub.pem -keyowner Acme -keyname acme-1 \
//               -control visibility=interface secret_core.sv
//   msim -keyfile Acme:acme-1=vendor.pem secret_core.svp core_tb.sv -vcd waves.vcd
`pragma protect begin
module secret_core(input logic clk, input logic [7:0] d, output logic [7:0] q);
  `pragma protect viewport
  logic [7:0] stage = 0;
  logic [7:0] hidden_sum = 0;
  always_ff @(posedge clk) begin
    hidden_sum <= hidden_sum + d;
    stage <= d ^ hidden_sum;
    q = stage;   // a blocking assignment in always_ff: an mlint finding inside the IP
  end
endmodule
`pragma protect end
