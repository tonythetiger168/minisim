// IEEE 1800-2023 24.3: a program nested in a module with no ports is
// instantiated once, implicitly, and shares the module's variables.
// 24.5: a module task called from a program runs in the reactive region
// set, so it reads the value the design's NBA wrote in this time step.
//! icarus-skip: Icarus 13 runs a program in the active region, like a module
module top;
  int shared = 0;
  logic clk = 0;
  logic [3:0] r = 0;
  always #5 clk = ~clk;
  always @(posedge clk) r <= r + 1;
  task show;
    $display("%0t task sees r=%0d", $time, r);
  endtask
  program p1;
    initial begin
      @(posedge clk) shared = 5;
      show();
    end
  endprogram
  program p2;
    initial begin
      @(posedge clk) #1 $display("%0t p2 sees shared=%0d", $time, shared);
    end
  endprogram
endmodule

//> 5 task sees r=1
//> 6 p2 sees shared=5
