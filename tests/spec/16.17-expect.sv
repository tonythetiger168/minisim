// IEEE 1800-2023 16.17: expect blocks its process until the property passes or
// fails, then runs the matching action; automatic variables are read when it starts.
//! icarus-skip: Icarus 13 does not support expect
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic a = 0, b = 0, c = 0;
  int phase = 0;
  // 16.17: expect blocks until the property passes or fails
  initial begin
    #12;
    expect (@(posedge clk) a ##1 b ##1 c) $display("%0t first passed", $time);
    else $display("%0t first failed", $time);
    phase = 1;
    expect (@(posedge clk) a ##1 b) $display("%0t second passed", $time);
    else $display("%0t second failed", $time);
    phase = 2;
    $display("%0t done", $time);
  end
  // automatic variables in expect are read when it starts (16.14.6.1)
  task automatic check(int v);
    expect (@(posedge clk) ##1 (phase == v)) $display("%0t phase %0d ok", $time, v);
    else $display("%0t phase %0d not reached", $time, v);
  endtask
  initial begin #3; check(1); end
  initial begin
    a = 1;
    @(negedge clk) a = 1;   // 10
    @(negedge clk) a = 0; b = 1;   // 20
    @(negedge clk) b = 0; c = 1;   // 30
    @(negedge clk) c = 0;          // 40: first passed at 35
    @(negedge clk);                // 50: second: a at 45 = 0 -> fail at 45
    @(negedge clk) $finish;
  end
endmodule

//> 15 phase 1 not reached
//> 35 first passed
//> 45 second failed
//> 45 done
