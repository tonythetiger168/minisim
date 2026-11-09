// mcov self-test design: +define+SEL=<n> picks which case arm runs.
`ifndef SEL
`define SEL 1
`endif
module dut(input logic clk, input logic [1:0] sel, output logic [3:0] y);
  always_ff @(posedge clk)
    case (sel)
      2'd0: y <= 4'd1;
      2'd1: y <= 4'd2;
      default: y <= 4'd8;
    endcase
endmodule
module tb;
  logic clk = 0;
  logic [1:0] sel = 0;
  logic [3:0] y;
  dut u(.clk(clk), .sel(sel), .y(y));
  always #5 clk = ~clk;
  initial begin
    #12 sel = `SEL;
    #10 if (y == 4'd2) $display("two"); else $display("other");
    $finish;
  end
endmodule
