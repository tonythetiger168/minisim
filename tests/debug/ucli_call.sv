// msim -i call, drivers / loads, save / restore / restart (tests/msim_ucli.cmake).
module tb;
  logic clk = 0;
  logic [7:0] acc = 0;
  int calls = 0;
  function int twice(input int v);
    return 2 * v;
  endfunction
  function automatic int addup(input int a, input int b);
    calls++;
    return a + b;
  endfunction
  task bump(input logic [7:0] by, output logic [7:0] now);
    acc = acc + by;
    now = acc;
  endtask
  always #5 clk = ~clk;
  always @(posedge clk) acc <= acc + 1;
  initial begin
    #100 $display("acc=%0d calls=%0d", acc, calls);
    $finish;
  end
endmodule
