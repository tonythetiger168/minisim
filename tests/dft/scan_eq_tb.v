// With scan_en low the scan-inserted netlist (top_scan) must behave exactly
// like the original (top): same inputs, random clocks, compare every output.
module scan_eq_tb;
  reg clk, clk2, rst_n, en;
  reg [1:0] din;
  integer i, errors;
  wire [3:0] c0, c1;
  wire d0, d1, q0, q1;
  top a (.clk(clk), .clk2(clk2), .rst_n(rst_n), .en(en), .din(din), .cnt(c0), .dout(d0), .q2(q0));
  top_scan b (.clk(clk), .clk2(clk2), .rst_n(rst_n), .en(en), .din(din), .cnt(c1), .dout(d1), .q2(q1),
              .scan_en(1'b0), .scan_in(3'b000), .scan_out());
  initial begin
    clk = 0; clk2 = 0; rst_n = 0; en = 0; din = 0; errors = 0;
    #2 rst_n = 1;
    for (i = 0; i < 400; i = i + 1) begin
      #1 din = $random; en = $random;
      #1 if ($random & 1) clk = ~clk;
      if ($random & 1) clk2 = ~clk2;
      #1 if (c0 !== c1 || d0 !== d1 || q0 !== q1) errors = errors + 1;
    end
    if (errors == 0 && c0 !== 4'd0) $display("SCAN EQ PASS");
    else $display("SCAN EQ FAIL: %0d errors, cnt=%h", errors, c0);
    $finish;
  end
endmodule
