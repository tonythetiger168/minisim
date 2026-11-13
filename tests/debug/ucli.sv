// msim -i UCLI commands (tests/msim_ucli.cmake): line stops, step and
// next, time and condition stops, scopes, describe, Tcl scripting, dump.
module counter #(parameter W = 4) (input logic clk, input logic en, output logic [W-1:0] q);
  initial q = 0;
  always @(posedge clk)
    if (en) q <= q + 1;
endmodule
module tb;
  logic clk = 0, en = 0;
  logic [3:0] q;
  int sum = 0;
  counter #(.W(4)) u1(.clk(clk), .en(en), .q(q));
  for (genvar g = 0; g < 2; g++) begin : lane
    logic [7:0] r = 8'(g);
  end
  task automatic add(input int v);
    sum = sum + v;
    sum = sum + 1;
  endtask
  always #5 clk = ~clk;
  initial begin
    #12 en = 1;
    add(3);
    #200 $display("q=%0d sum=%0d", q, sum);
    $finish;
  end
endmodule
