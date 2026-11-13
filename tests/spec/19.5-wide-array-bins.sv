// IEEE 1800-2023 19.5.1: array bins of a coverpoint wider than 64 bits.
// name[] makes one bin per value; name[N] spreads the values, in the order
// written, over N bins, the last one taking the rest.
//! icarus-skip: Icarus has no covergroups
module t;
  bit [99:0] v;
  logic signed [71:0] s;
  covergroup cg;
    cv: coverpoint v {
      bins one[] = {100'h1_0000_0000_0000_0000_0, [100'h5 : 100'h7]};
      bins quarter[4] = {[100'h8_0000_0000_0000_0000_0 : 100'hf_ffff_ffff_ffff_ffff_ffff_ffff]};
    }
    cs: coverpoint s {
      bins split[3] = {[$ : -2], 0, [72'sd10 : 72'sd11]};
    }
  endgroup
  cg c = new;
  initial begin
    v = 6; s = -72'sd5; c.sample();
    $display("%.2f %.2f", c.cv.get_coverage(), c.cs.get_coverage());
    v = 100'h1_0000_0000_0000_0000_0; s = 0; c.sample();
    $display("%.2f %.2f", c.cv.get_coverage(), c.cs.get_coverage());
    v = 100'h9_0000_0000_0000_0000_0; s = 72'sd11; c.sample();
    $display("%.2f %.2f", c.cv.get_coverage(), c.cs.get_coverage());
    v = 100'hf_0000_0000_0000_0000_0000_0000; s = 72'sd10; c.sample();
    $display("%.2f %.2f", c.cv.get_coverage(), c.cs.get_coverage());
    v = 100'h4; s = 72'sh80_0000_0000_0000_0000; c.sample();   // the lowest value: split[0]
    $display("%.2f %.2f", c.cv.get_coverage(), c.cs.get_coverage());
  end
endmodule
//> 12.50 33.33
//> 25.00 33.33
//> 37.50 33.33
//> 50.00 33.33
//> 50.00 66.67
