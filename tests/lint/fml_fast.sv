// The formal-assisted checks with and without their shortcuts must agree
// (tests/mlint.cmake runs this with formal.fast=1 and =0): whole-case item
// checks, independent path conditions, and the case-item cache.
module fml_fast (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [11:0] addr,
    input  logic [3:0]  op,
    input  logic [2:0]  sel,
    input  logic [2:0]  k,
    input  logic        a,
    input  logic        b,
    input  logic [7:0]  d,
    output logic [7:0]  y,
    output logic [7:0]  z,
    output logic [3:0]  w,
    output logic        o
);
  logic [7:0] mem[0:5];
  logic [7:0] tab[0:3];
  logic [2:0] cnt;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) cnt <= 0;
    else cnt <= (cnt == 3'd3) ? 3'd0 : cnt + 3'd1;
  always_ff @(posedge clk) mem[k] <= d;
  always_comb begin
    y = 8'd0;
    z = 8'd0;
    o = 1'b0;
    w = 4'd0;
    // a wide selector, items with duplicates and ranges of the same value
    case (addr)
      12'h300: y = mem[sel];
      12'h301: y = tab[cnt];
      12'h300: y = 8'd1;
      12'h7c0, 12'h7c1: y = mem[k];
      12'h7c1: y = 8'd2;
      12'hf11: if (a) y = tab[sel[1:0]];
      default: y = 8'd0;
    endcase
    // a path condition that has nothing to do with the index
    if (op == 4'd3)
      if (b) z = mem[sel];
      else z = mem[cnt];
    // casez with wildcards, nested in a condition on another signal
    if (a)
      casez (op)
        4'b1???: w = 4'd1;
        4'b10??: w = 4'd2;
        4'b0001: w = tab[sel][3:0];
        4'b???1: w = 4'd3;
        default: w = 4'd4;
      endcase
    // items that are not constants
    case (sel)
      k: o = 1'b1;
      3'd7: o = a;
      k: o = 1'b0;
      default: o = 1'b0;
    endcase
    // the same selector twice: the inner case under the outer item
    case (op)
      4'd1: case (op)
              4'd1: z = mem[sel];
              4'd2: z = 8'd5;
              default: z = 8'd6;
            endcase
      4'd2: z = (sel < 3'd6) ? mem[sel] : 8'd0;
      default: ;
    endcase
  end
endmodule
