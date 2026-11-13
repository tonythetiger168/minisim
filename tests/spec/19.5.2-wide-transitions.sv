// IEEE 1800-2023 19.5.2: transition bins of a coverpoint wider than 64
// bits, with single values, ranges and repetition, compared on the full
// width.
//! icarus-skip: Icarus has no covergroups
module t;
  bit [79:0] st;
  bit clk;
  covergroup cg @(posedge clk);
    coverpoint st {
      bins up    = (80'h1_0000_0000_0000_0000 => 80'h1_0000_0000_0000_0001);
      bins range = ([0 : 9] => [80'hf0_0000_0000_0000_0000 : $]);
      bins hold  = (80'h2_0000_0000_0000_0000 [* 3]);
      bins seq3  = (1 => 2 => 80'h3_0000_0000_0000_0000);
    }
  endgroup
  cg c = new;
  bit [79:0] trace[] = '{80'h1_0000_0000_0000_0000, 80'h1_0000_0000_0000_0001, 5,
                         80'hff_0000_0000_0000_0000, 80'h2_0000_0000_0000_0000,
                         80'h2_0000_0000_0000_0000, 80'h2_0000_0000_0000_0000,
                         1, 2, 80'h3_0000_0000_0000_0001};
  initial begin
    foreach (trace[i]) begin
      st = trace[i];
      #1 clk = 1;
      #1 clk = 0;
      if (i == 1 || i == 3 || i == 6 || i == 9) $display("%0d: %.2f", i, c.get_coverage());
    end
  end
endmodule
//> 1: 25.00
//> 3: 50.00
//> 6: 75.00
//> 9: 75.00
