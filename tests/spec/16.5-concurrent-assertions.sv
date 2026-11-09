// IEEE 1800-2023 16.5: A concurrent assertion with an implication, checked
// on each clock (16.5, 16.12).
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  logic clk = 0, req = 0, ack = 0;
  always #5 clk = ~clk;
  assert property (@(posedge clk) req |=> ack) else $display("fail at %0t", $time);
  initial begin
    @(negedge clk) req = 1;
    @(negedge clk) begin req = 0; ack = 1; end
    @(negedge clk) begin req = 1; ack = 0; end
    @(negedge clk) req = 0;
    @(negedge clk) $finish;
  end
endmodule

//> fail at 45
