// IEEE 1800-2023 7.4 with 9.2.2.2 always_comb: a read through a variable index
// wakes the block when any element or the index changes.
module t;
  logic [7:0] mem [4];
  logic [1:0] a;
  logic [7:0] q;
  always_comb q = mem[a];
  initial begin
    mem[0] = 1; mem[1] = 2; mem[2] = 3; mem[3] = 4;
    a = 1;
    #1 $display("%0d", q);
    mem[1] = 9;
    #1 $display("%0d", q);
    a = 3;
    #1 $display("%0d", q);
  end
endmodule
//> 2
//> 9
//> 4
