// IEEE 1800-2023 19.6: a cross may take a coverpoint wider than 64 bits.
// Its automatic cross bins are the tuples of the coverpoints' bins; user
// cross bins select with binsof, also with intersect on the full width.
//! icarus-skip: Icarus has no covergroups
module t;
  bit [95:0] key;
  bit [1:0] op;
  covergroup cg;
    k: coverpoint key {
      bins lo_k = {[0 : 255]};
      bins hi_k = {[96'h1_0000_0000_0000_0000 : $]};
    }
    o: coverpoint op { bins rd = {0}; bins wr = {1}; }
    x: cross k, o;
    y: cross k, o {
      bins hi_wr = binsof(k) intersect {[96'h1_8000_0000_0000_0000 : 96'h2_0000_0000_0000_0000]} && binsof(o.wr);
      ignore_bins lo = binsof(k.lo_k);
    }
  endgroup
  cg c = new;
  initial begin
    key = 3; op = 0; c.sample();
    $display("%.2f %.2f", c.x.get_coverage(), c.y.get_coverage());
    key = 96'h1_9000_0000_0000_0000; op = 1; c.sample();
    $display("%.2f %.2f", c.x.get_coverage(), c.y.get_coverage());
    key = 96'hffff_0000_0000_0000_0000; op = 0; c.sample();
    $display("%.2f %.2f", c.x.get_coverage(), c.y.get_coverage());
  end
endmodule
//> 25.00 0.00
//> 50.00 50.00
//> 75.00 100.00
