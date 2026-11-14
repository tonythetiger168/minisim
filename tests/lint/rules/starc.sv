//! args: -only CASE005,CASE006,CASE007,SYN018,SYN019,SYN020,W010,EXP003,EXP005,EXP006,CLK004 -top starc
module starc (
    input logic clk,
    input logic rst_n,
    input logic [3:0] a,
    input logic [3:0] b,
    input logic [1:0] sel,
    input logic s1,
    output logic [3:0] y,
    output logic [3:0] z,
    output logic q2,
    output logic flag
);
  integer i;
  int count; //~ SYN020
  logic div2;
  logic [3:0] m;

  task automatic set_y(output logic [3:0] o, input logic [3:0] v);
    o = v;
  endtask

  always_comb begin
    m = '0;
    for (i = 0; i < 4; i++) m[i] = a[i] ^ b[i];
    case (sel)
      2'd0: y = a;
      2'd1: y = b;
      {1'b1, s1}: y = m; //~ CASE005
      default: y = '0;
    endcase
    case (1'b1) //~ CASE007
      a[0]: z = 4'd1;
      a[1]: z = 4'd2;
      default: z = 4'd0;
    endcase
    case (sel) // synopsys full_case parallel_case //~ CASE006 CASE006
      2'd0: flag = 1'b0;
      2'd1: flag = 1'b1;
    endcase
    if (a < 0) flag = 1'b0; //~ EXP005
    if (a >= 0) flag = |s1; //~ EXP005 EXP003
    if (b > 4'd15) flag = 1'b1; //~ EXP005
    if (b > 4'd14) flag = 1'b1;
    if (1'b0) flag = (a << 4) != 0; //~ EXP006 W010
    if (a >> 3 != 0) flag = 1'b0;
    count = 0;
    while (count < 3) count++; //~ SYN018
    set_y(y, a); //~ SYN019
  end

  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) div2 <= 1'b0;
    else div2 <= ~div2;

  always_ff @(posedge div2) q2 <= a[0]; //~ CLK004
endmodule
