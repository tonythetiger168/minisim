// IEEE 1800-2023 14.3: A clocking block samples inputs at the clock and
// drives outputs after it (14.3, 14.13, 14.16).
//! icarus-skip: Icarus 13 does not support clocking blocks
module top;
  logic clk = 0, d = 0, q;
  always #5 clk = ~clk;
  clocking cb @(posedge clk);
    input d;
    output q;
  endclocking
  initial begin
    #3 d = 1;
    @(cb);
    $display("%0t %b", $time, cb.d);
    cb.q <= 1;
    @(cb);
    $display("%0t %b", $time, q);
    $finish;
  end
endmodule

//> 5 1
//> 15 1
