// IEEE 1800-2023 24.3.1, 24.4: a program's code runs in the reactive region,
// after the design's NBA updates in the same time step, so it reads the
// values the design settles on. A module process woken by the same clock
// edge reads the old value.
//! icarus-skip: Icarus 13 runs a program in the active region, like a module
module top;
  logic clk = 0;
  logic [7:0] q = 0;
  always #5 clk = ~clk;
  always @(posedge clk) q <= q + 1;
  always @(posedge clk) $display("%0t module sees %0d", $time, q);
  tb t();
endmodule

program tb;
  initial begin
    repeat (3) begin
      @(posedge top.clk);
      $display("%0t program sees %0d", $time, top.q);
    end
  end
endprogram

//> 5 module sees 0
//> 5 program sees 1
//> 15 module sees 1
//> 15 program sees 2
//> 25 module sees 2
//> 25 program sees 3
