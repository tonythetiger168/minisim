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

  // A two-phase handshake: hreq toggles once the acknowledge has come back.
  logic hreq, ha1, ha2, hr1, hr2, hack;
  always_ff @(posedge fclk or negedge rst_n)
    if (!rst_n) hreq <= 0; else if (hreq == ha2) hreq <= ~hreq;
  always_ff @(posedge sclk or negedge rst_n)
    if (!rst_n) begin hr1 <= 0; hr2 <= 0; hack <= 0; end else begin hr1 <= hreq; hr2 <= hr1; hack <= hr2; end
  always_ff @(posedge fclk or negedge rst_n)
    if (!rst_n) begin ha1 <= 0; ha2 <= 0; end else begin ha1 <= hack; ha2 <= ha1; end

  // An impatient one: breq also toggles every 16 fast cycles, acknowledged
  // or not.
  logic breq, ba1, ba2, br1, br2, back;
  always_ff @(posedge fclk or negedge rst_n)
    if (!rst_n) breq <= 0; else if (breq == ba2 || bin == 4'h7) breq <= ~breq;
  always_ff @(posedge sclk or negedge rst_n)
    if (!rst_n) begin br1 <= 0; br2 <= 0; back <= 0; end else begin br1 <= breq; br2 <= br1; back <= br2; end
  always_ff @(posedge fclk or negedge rst_n)
    if (!rst_n) begin ba1 <= 0; ba2 <= 0; end else begin ba1 <= back; ba2 <= ba1; end
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
