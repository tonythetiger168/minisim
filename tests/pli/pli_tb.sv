// The IP user's testbench: its own VPI task probes the protected instance.
module pli_tb;
  import "DPI-C" context function void dpi_where(input int tag);
  import "DPI-C" context function void dpi_lookup();
  logic clk = 0;
  logic [7:0] d = 1;
  wire [7:0] q;
  secret_pli u(.clk(clk), .d(d), .q(q));
  always #5 clk = ~clk;
  initial begin
    #10 $probe;
    dpi_where(2);
    dpi_lookup();
    #30 $finish;
  end
endmodule
