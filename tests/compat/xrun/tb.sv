// xrun front end test (tests/msim_xrun.cmake): a macro from -define, a
// header from -incdir, a module from a -F file, a plusarg.
`include "w.svh"
module tb;
  logic clk = 0;
  int n = 0;
  leaf u_leaf();
  always #5 clk = ~clk;
  always @(posedge clk) n <= n + 1;
  initial begin
    $display("%s W=%0d seed-ok", `GREETING, `W);
    if ($test$plusargs("stop")) begin #100 $display("n=%0d at %0t", n, $time); $finish; end
  end
endmodule
