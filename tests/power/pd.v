// Two blocks for the UPF test: u_aon is always on, u_core is behind a power
// switch that sw_en closes.
module blk(input clk, input d, output q);
  wire n;
  INV u1(.A(d), .Y(n));
  DFF r(.D(n), .CK(clk), .Q(q));
endmodule
module pd(input clk, input d, input sw_en, output q0, output q1);
  blk u_aon(.clk(clk), .d(d), .q(q0));
  blk u_core(.clk(clk), .d(d), .q(q1));
endmodule
