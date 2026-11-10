// A small design for the msdc self-test: two clock domains with a
// synchronizer between them, a divided clock, a clock mux, a clock nobody
// constrains, and ports of every kind.
module sync2(input logic clk, input logic d, output logic q);
  logic m;
  always_ff @(posedge clk) m <= d;
  always_ff @(posedge clk) q <= m;
endmodule

module core(input logic clk, input logic rst_n, input logic [7:0] din, input logic en,
            output logic [7:0] dout, output logic flag);
  logic [7:0] acc;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) acc <= '0;
    else if (en) acc <= acc + din;
  assign dout = acc;
  always_ff @(posedge clk) flag <= &acc;
endmodule

module soc(input logic clk_a, clk_b, clk_x, input logic rst_n, input logic test_mode, input logic scan_clk,
           input logic [7:0] din, input logic en, input logic b_in, input logic unused_in,
           output logic [7:0] dout, output logic flag_b, output logic div_out, output logic x_out,
           output logic comb_out);
  // domain A
  logic flag_a;
  core u_core(.clk(clk_a), .rst_n(rst_n), .din(din), .en(en), .dout(dout), .flag(flag_a));

  // A to B through a synchronizer, and once directly
  logic flag_sync, raw_b;
  sync2 u_sync(.clk(clk_b), .d(flag_a), .q(flag_sync));
  always_ff @(posedge clk_b) flag_b <= flag_sync & b_in;
  always_ff @(posedge clk_b) raw_b <= flag_a;

  // a divided clock
  logic div2;
  always_ff @(posedge clk_a) div2 <= ~div2;
  logic slow;
  always_ff @(posedge div2) slow <= flag_a;
  assign div_out = slow;

  // a clock mux
  logic mclk;
  assign mclk = test_mode ? scan_clk : clk_b;
  logic muxed;
  always_ff @(posedge mclk) muxed <= raw_b;

  // a clock with no constraint
  logic xr;
  always_ff @(posedge clk_x) xr <= muxed;
  assign x_out = xr;

  assign comb_out = b_in ^ en;
endmodule
