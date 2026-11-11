// -connect: a small pad multiplexer.
module periph(input logic clk, input logic [7:0] tx, output logic [7:0] q);
  always_ff @(posedge clk) q <= tx;
endmodule
module connect(input logic clk, input logic rst, input logic [1:0] mode, input logic test_en,
               input logic [7:0] a, b, scan, output logic [7:0] pad, output logic [3:0] dbg);
  logic [7:0] func;
  logic [7:0] u_q[2];
  for (genvar i = 0; i < 2; i++) begin : g
    periph u(.clk, .tx(i == 0 ? a : b), .q(u_q[i]));
  end
  always_comb
    case (mode)
      2'd0: func = a;
      2'd1: func = b;
      2'd2: func = a ^ b;       // the spec says a & b: a wrong connection
      default: func = '0;
    endcase
  assign pad = test_en ? scan : func;
  assign dbg = pad[7:4];
  m_mode: assume property (@(posedge clk) mode != 2'd3);
endmodule
