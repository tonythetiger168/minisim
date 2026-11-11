// A small pad ring for the boundary-scan test: a clock, plain inputs and
// outputs, one bidirectional bus built from out/enable/in ports, and a
// bidirectional pin with no enable the tool can see.
module chip(input logic clk, rst_n, input logic [3:0] a, input logic en,
            output logic [3:0] y, output logic q,
            output logic [1:0] d_o, output logic d_oe, input logic [1:0] d_i,
            inout wire aux);
  logic [3:0] r;
  always_ff @(posedge clk or negedge rst_n) if (!rst_n) r <= '0; else if (en) r <= a;
  assign y = r ^ {2'b0, d_i};
  assign q = ^r;
  assign d_o = r[1:0];
  assign d_oe = r[3];
endmodule
