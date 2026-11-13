// A register block shaped like a generated one: a packed struct of fields,
// each written by its own instance in its own clock domain (through a port
// on the field, or on a concatenation of fields), copied whole with an
// assign and passed whole down a port. Only the field that crosses is
// reported. The b-domain clock goes through a scan mux, which is still clk_b.
typedef struct packed {
  logic       a;   // clk_a
  logic       b;   // clk_b
  logic [1:0] c;   // clk_a, read in clk_b: the one crossing
  logic       d;   // clk_b
} regs_t;

module cdc_regs_field #(parameter int W = 1) (
  input  logic         clk,
  input  logic [W-1:0] d,
  output logic [W-1:0] q
);
  always_ff @(posedge clk) q <= d;
endmodule

module cdc_regs_core (
  input  logic  clk,
  input  regs_t r,
  output logic  q
);
  always_ff @(posedge clk) q <= r.b ^ r.d;
endmodule

module cdc_regs_cfg (
  input  logic  clk,
  input  regs_t r,
  output logic  q
);
  always_ff @(posedge clk) q <= ^r.c;
endmodule

module cdc_regs (
  input  logic       clk_a,
  input  logic       clk_b_func,
  input  logic       scan_clk_i,
  input  logic       scanmode_i,
  input  logic [4:0] x,
  output logic       q, qc
);
  logic clk_b;
  assign clk_b = scanmode_i ? scan_clk_i : clk_b_func;
  regs_t hw, hw_top;
  cdc_regs_field u_a(.clk(clk_a), .d(x[0]), .q(hw.a));
  cdc_regs_field #(.W(2)) u_bd(.clk(clk_b), .d(x[2:1]), .q({hw.b, hw.d}));
  cdc_regs_field #(.W(2)) u_c(.clk(clk_a), .d(x[4:3]), .q(hw.c));
  assign hw_top = hw;
  cdc_regs_core u_core(.clk(clk_b), .r(hw_top), .q(q));
  cdc_regs_cfg  u_cfg(.clk(clk_b), .r(hw_top), .q(qc));
endmodule
