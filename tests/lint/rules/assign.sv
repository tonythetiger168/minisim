//! args: -only assign,driver -top asg
module asg(input logic clk, a, b, output logic y, z, w, v, m, n);
  logic t, mix;                   //~ SEQ001
  logic tmp;
  always @(posedge clk) begin     //~ SEQ002
    y = a;                        // read by the block below
    tmp = a & b;                  // a temporary of this block only
    z <= tmp;
  end
  always @(posedge clk) w <= y;
  always @* v <= a | b;           //~ SEQ003
  always @(posedge clk) mix <= a;
  always @* mix = b;              //~ L004
  always_comb begin
    t = a;
    t = t;                        //~ EXP001
  end
  always_ff @(posedge clk) begin  //~ L002 L004
    y = b;
  end
  always_comb n <= t;             //~ L003
  assign m = a;
  assign m = b;                   //~ L004
endmodule
