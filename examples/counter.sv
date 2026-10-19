// 4-bit counter with synchronous reset (used to try slang_dump)
module counter(input logic clk, rst, output logic [3:0] cnt);
  always_ff @(posedge clk)
    if (rst) cnt <= 0;
    else     cnt <= cnt + 1;
endmodule
