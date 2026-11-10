// msim -xprop: writes to an array element chosen at run time, inside an
// unknown if. The element takes the merge of its present and its new value.
//! args: -xprop
module top;
  logic clk = 0, we, c;
  logic [1:0] a, b;
  logic [7:0] d, mem [4], cm [4], pm [4], twice [4];
  always @(posedge clk) if (we) mem[a] <= d;
  always_comb begin
    for (int i = 0; i < 4; i++) cm[i] = 8'h11 * i;
    if (c) cm[a] = d; else cm[b] = 8'hff;
  end
  always @(posedge clk) begin
    if (we) pm[a][3:0] <= d[3:0];
    else pm[b] <= 8'h33;
  end
  // Two nonblocking writes in one pass: the second cannot merge with the first, so it gives x.
  always @(posedge clk) begin
    twice[a] <= 8'h00;
    if (we) twice[a] <= 8'h00;
  end
  task show;
    $display("mem=%h %h %h %h cm=%h %h %h %h pm=%h %h %h %h twice=%h", mem[0], mem[1], mem[2], mem[3],
             cm[0], cm[1], cm[2], cm[3], pm[0], pm[1], pm[2], pm[3], twice[a]);
  endtask
  initial begin
    for (int i = 0; i < 4; i++) begin mem[i] = 8'hA0 + i; pm[i] = 8'h30 + i; end
    we = 1; c = 1; a = 1; b = 2; d = 8'hA5;
    #1 clk = 1; #1 clk = 0; show;
    we = 1'bx; c = 1'bx; a = 2; b = 3; d = 8'hA6;
    #1 clk = 1; #1 clk = 0; show;
    d = 8'hA2; #1 show;
    we = 0; c = 0;
    #1 clk = 1; #1 clk = 0; show;
    $finish;
  end
endmodule
//> mem=a0 a5 a2 a3 cm=00 a5 22 33 pm=30 35 32 33 twice=00
//> mem=a0 a5 aX a3 cm=00 11 XX XX pm=30 35 3X 33 twice=xx
//> mem=a0 a5 aX a3 cm=00 11 X2 XX pm=30 35 3X 33 twice=xx
//> mem=a0 a5 aX a3 cm=00 11 22 ff pm=30 35 3X 33 twice=00
