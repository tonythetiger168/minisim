// A testbench's clocked block with $display and $finish (seqShape sys):
// with -compile it runs in the cycle domain, printing where it runs and
// reading the values from before the edge. Against the interpreter.
module seqsys_core (input logic clk, output logic [7:0] cnt);
  logic [7:0] c = 0;
  always_ff @(posedge clk) c <= c + 8'd3;
  assign cnt = c;
endmodule
module seqsys;
  logic clk = 0;
  logic [7:0] mem [0:7];
  logic [7:0] seen = 0, last;
  wire  [7:0] cnt;
  seqsys_core u (.clk, .cnt);
  always #5 clk = ~clk;
  always_ff @(posedge clk) begin
    seen <= cnt;
    mem[cnt[2:0]] <= cnt;
    if (cnt[0]) $display("odd cnt=%0d seen=%0d", cnt, seen);
    else $write("even cnt=%0d ", cnt);
    if (cnt > 8'd20) begin
      $display("done mem[1]=%0d mem[4]=%0d", mem[1], mem[4]);
      $finish;
    end
  end
endmodule
