// msim -xprop: a write to an array element whose index has an unknown bit.
// Without -xprop the write is skipped (7.4.6). With it, every element the
// index may name takes the merge of its present value and the new one.
//! args: -xprop
module top;
  logic clk = 0, we;
  logic [1:0] a;
  logic [7:0] d, mem [4], cm [4], rev [3:0];
  always @(posedge clk) if (we) mem[a] <= d;
  always_comb begin
    for (int i = 0; i < 4; i++) cm[i] = 8'h10 + i;
    cm[a] = 8'h1f;
  end
  always_comb begin
    for (int i = 0; i < 4; i++) rev[i] = 8'h20 + i;
    rev[a] = 8'h2f;
  end
  initial begin
    for (int i = 0; i < 4; i++) mem[i] = 8'hA0 + i;
    we = 1; a = 2'b1x; d = 8'hA5;
    #1 clk = 1; #1 clk = 0;
    $display("mem=%h %h %h %h cm=%h %h %h %h rev=%h %h %h %h", mem[0], mem[1], mem[2], mem[3],
             cm[0], cm[1], cm[2], cm[3], rev[0], rev[1], rev[2], rev[3]);
    a = 2'bx1; d = 8'hAF; #1 clk = 1; #1 clk = 0;
    $display("mem=%h %h %h %h cm=%h %h %h %h", mem[0], mem[1], mem[2], mem[3], cm[0], cm[1], cm[2], cm[3]);
    a = 2'b01; #1 clk = 1; #1 clk = 0;
    $display("mem=%h %h %h %h", mem[0], mem[1], mem[2], mem[3]);
    $finish;
  end
endmodule
//> mem=a0 a1 aX aX cm=10 11 1X 1X rev=20 21 2X 2X
//> mem=a0 aX aX aX cm=10 1X 12 1X
//> mem=a0 af aX aX
