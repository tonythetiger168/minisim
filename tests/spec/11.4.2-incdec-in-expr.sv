// IEEE 1800-2023 11.4.2: i++ and --i inside an expression; 11.4.6: ==? and !=?
module top;
  integer i = 2, j;
  logic [3:0] w;
  initial begin
    $display("%0d", i++ + 10);
    $display("%0d", i);
    $display("%0d", --i * 2);
    j = 0;
    while (j++ < 3) $display("loop %0d", j);
    w = 4'b10x0;
    $display("%b %b", w ==? 4'b10x0, 4'b1001 !=? 4'b1z0z);
  end
endmodule

//> 12
//> 3
//> 4
//> loop 1
//> loop 2
//> loop 3
//> 1 0
