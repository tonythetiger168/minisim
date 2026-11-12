// mlint -cdc-sva on a running design: the Gray counter and the slow request
// hold their assertions; the fast toggle into the slow clock breaks its own.
module cdc_sva(input logic fclk, sclk, rst_n, output logic [3:0] g_s, output logic t_s, r_s);
  // A Gray counter in the fast domain, synchronized into the slow one.
  logic [3:0] bin, gray, g1;
  always_ff @(posedge fclk or negedge rst_n)
    if (!rst_n) begin bin <= '0; gray <= '0; end
    else begin bin <= bin + 1'b1; gray <= (bin + 1'b1) ^ ((bin + 1'b1) >> 1); end
  always_ff @(posedge sclk) begin g1 <= gray; g_s <= g1; end

  // A toggle every fast cycle: too fast for the slow clock.
  logic tog, t1;
  always_ff @(posedge fclk or negedge rst_n) if (!rst_n) tog <= 0; else tog <= ~tog;
  always_ff @(posedge sclk) begin t1 <= tog; t_s <= t1; end

  // A request that changes every 16 fast cycles: slow enough.
  logic req, r1;
  always_ff @(posedge fclk or negedge rst_n) if (!rst_n) req <= 0; else if (bin == 4'hf) req <= ~req;
  always_ff @(posedge sclk) begin r1 <= req; r_s <= r1; end
endmodule

module cdc_sva_tb;
  logic fclk = 0, sclk = 0, rst_n = 0;
  logic [3:0] g_s;
  logic t_s, r_s;
  cdc_sva dut(.*);
  always #2 fclk = ~fclk;
  always #5 sclk = ~sclk;
  initial begin
    #7 rst_n = 1;
    #400 $finish;
  end
endmodule
