// msim -compile mixed mode: the design's clocked and combinational logic is
// compiled; the testbench's class, automatic task, queue and fork run in
// msim's interpreter beside it (codegen.hpp Codegen::mix). The output is the
// same either way.
module counter (input logic clk, rst, en, output logic [7:0] q, output logic odd);
  always_ff @(posedge clk) if (rst) q <= 0; else if (en) q <= q + 1;
  assign odd = q[0];
endmodule

class Scoreboard;
  int seen[$];
  function void put(int v); seen.push_back(v); endfunction
  function int sum();
    int s = 0;
    foreach (seen[i]) s += seen[i];
    return s;
  endfunction
endclass

module mixed;
  logic clk = 0, rst = 1, en = 0;
  logic [7:0] q;
  logic odd;
  counter u (.*);
  always #5 clk = ~clk;
  Scoreboard sb = new;

  task automatic pulse(input int n);
    repeat (n) @(negedge clk);   // inputs change away from the active edge
  endtask

  always @(posedge clk) if (!rst && odd) sb.put(q);

  initial begin
    pulse(2);
    rst = 0;
    en = 1;
    fork
      pulse(20);
      begin pulse(5); $display("[%0t] q=%0d after 5", $time, q); end
    join
    en = 0;
    pulse(2);
    $display("q=%0d odd samples=%0d sum=%0d", q, sb.seen.size(), sb.sum());
    $finish;
  end
  final $display("final: q=%0d", q);
endmodule
