// Hierarchy for mpopt: two parameterizations of one module, a generate
// loop, a synchronous reset, and Verilog-2001 style code.
module stage #(parameter W = 8) (
  input              clk,
  input              rst,
  input              en,
  input      [W-1:0] d,
  output reg [W-1:0] q,
  output reg [W-1:0] q2
);
  always @(posedge clk) begin
    if (rst) q <= {W{1'b0}};
    else if (en) q <= d;
  end
  always @(posedge clk) begin
    if (rst) q2 <= {W{1'b0}};
    else q2 <= q ^ {W{1'b1}};
  end
endmodule

module top (
  input         clk,
  input         rst,
  input  [3:0]  en,
  input  [7:0]  d,
  input  [1:0]  sel,
  output [7:0]  o8,
  output [15:0] o16,
  output reg [7:0] r
);
  wire [7:0] q8, q8b;
  wire [15:0] q16, q16b;
  stage #(.W(8))  u8  (.clk(clk), .rst(rst), .en(en[0]), .d(d), .q(q8), .q2(q8b));
  stage #(.W(16)) u16 (.clk(clk), .rst(rst), .en(en[1]), .d({d, d}), .q(q16), .q2(q16b));
  assign o8 = q8 ^ q8b;
  assign o16 = q16 ^ q16b;

  genvar i;
  generate
    for (i = 0; i < 2; i = i + 1) begin : g
      reg [7:0] t;
      always @(posedge clk)
        if (en[2 + i]) t <= d + i;
    end
  endgenerate

  always @(posedge clk) begin
    if (rst) r <= 8'd0;
    else begin
      case (sel)
        2'd0: r <= g[0].t;
        2'd1: r <= g[1].t;
        2'd2: ;
        default: r <= r + 8'd1;
      endcase
    end
  end
endmodule
