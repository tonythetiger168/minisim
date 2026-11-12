// mcov exclude -formal: dead can never be 1, so its toggle coverage cannot
// close; mformal -app toggle proves it and mcov turns that into an exclusion.
module unreach(input logic clk, rst, input logic a);
  logic [3:0] cnt;
  logic dead;
  always_ff @(posedge clk)
    if (rst) begin cnt <= 0; dead <= 0; end
    else begin cnt <= cnt + {3'b0, a}; dead <= cnt[0] & ~cnt[0]; end
endmodule

module unreach_tb;
  logic clk = 0, rst = 1, a = 0;
  unreach dut(.clk, .rst, .a);
  always #5 clk = ~clk;
  initial begin
    #12 rst = 0; a = 1;
    #40 a = 0;
    #20 $finish;
  end
endmodule
