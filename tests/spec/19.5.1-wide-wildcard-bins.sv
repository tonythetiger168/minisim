// IEEE 1800-2023 19.5.1: wildcard bins of a coverpoint wider than 64 bits.
// An x, z or ? bit of a wildcard bin value matches 0 and 1, on every bit
// of the value; wildcard ignore_bins and illegal_bins work the same way.
//! icarus-skip: Icarus has no covergroups
module t;
  bit [95:0] a;
  covergroup cg;
    coverpoint a {
      wildcard bins top_f = {96'hf???_????_????_????_????_????};
      wildcard bins odd   = {96'b?1};
      wildcard bins mid   = {96'h????_????_ab??_????_????_????};
      wildcard ignore_bins low_zero = {96'h????_????_????_????_????_0000};
      wildcard illegal_bins bad = {96'h0bad_????_????_????_????_????};
    }
  endgroup
  cg c = new;
  initial begin
    a = 96'hf000_0000_0000_0000_0000_0010; c.sample();
    $display("%.2f", c.get_coverage());
    a = 96'h0000_0000_0000_0000_0000_0000; c.sample();   // ignored (low 16 bits 0)
    $display("%.2f", c.get_coverage());
    a = 96'h1234_5678_9abc_def0_1234_5679; c.sample();   // odd
    $display("%.2f", c.get_coverage());
    a = 96'h0000_0000_ab00_0000_0000_0002; c.sample();   // mid
    $display("%.2f", c.get_coverage());
    a = 96'h0bad_0000_0000_0000_0000_0002; c.sample();
  end
endmodule
//> 33.33
//> 33.33
//> 66.67
//> 100.00
//> Error: covergroup t.cg: illegal bin a.bad hit (value 3613479274828126603196760066) at time 0
