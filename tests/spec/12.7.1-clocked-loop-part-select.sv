// IEEE 1800-2023 12.7.1, 11.5.1: a for loop in a clocked process that
// writes one byte lane of a memory word per pass, with an indexed part
// select whose base uses the loop variable (a byte-enable write).
module top;
  logic clk = 0;
  logic [31:0] mem [0:3];
  logic [31:0] r;
  logic [3:0]  be;
  logic [31:0] wd;
  always @(posedge clk) begin
    for (int b = 0; b < 4; b++)
      if (be[b]) mem[1][b*8 +: 8] <= wd[b*8 +: 8];
    for (int b = 0; b < 4; b++)
      r[b*8 +: 8] <= wd[(3-b)*8 +: 8];
  end
  initial begin
    be = 4'hf; wd = 32'hdeadbeef;
    #1 clk = 1; #1 clk = 0;
    #1 $display("%h %h", mem[1], r);
    be = 4'h5; wd = 32'h11223344;
    #1 clk = 1; #1 clk = 0;
    #1 $display("%h %h", mem[1], r);
  end
endmodule

//> deadbeef efbeadde
//> de22be44 44332211
