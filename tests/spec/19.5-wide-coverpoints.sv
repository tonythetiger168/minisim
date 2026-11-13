// IEEE 1800-2023 19.5: a coverpoint may be wider than 64 bits. Explicit
// value bins with single values and ranges (also open with $), ignore_bins,
// illegal_bins and default work on the full width, signed or not, and so
// do automatic bins (19.5.3): 2^w values in auto_bin_max equal parts.
//! icarus-skip: Icarus has no covergroups
module t;
  bit [127:0] addr;
  logic signed [79:0] off;
  covergroup cg;
    coverpoint addr {
      bins low   = {[0 : 128'hffff]};
      bins top   = {128'hffff_ffff_ffff_ffff_0000_0000_0000_0000, [128'hffff_ffff_ffff_ffff_ffff_ffff_ffff_fff0 : $]};
      bins mid   = {[128'h1_0000_0000_0000_0000 : 128'h2_0000_0000_0000_0000]};
      ignore_bins skip = {128'h1_8000_0000_0000_0000};
      bins other = default;
    }
    coverpoint off {
      bins neg = {[$ : -1]};
      bins zero = {0};
      bins big = {[80'sh1_0000_0000_0000_0000 : $]};
      illegal_bins bad = {80'sh10, -80'sd7};
    }
    a4: coverpoint t.addr { option.auto_bin_max = 4; }
    s3: coverpoint t.off { option.auto_bin_max = 3; }
  endgroup
  cg c = new;
  initial begin
    addr = 5; off = -80'sd5; c.sample();
    $display("%.2f %.2f %.2f", c.addr.get_coverage(), c.off.get_coverage(), c.get_coverage());
    addr = 128'h1_8000_0000_0000_0000; off = 0; c.sample();   // ignored: mid stays empty
    $display("%.2f %.2f", c.addr.get_coverage(), c.off.get_coverage());
    addr = 128'h1_8000_0000_0000_0001; off = 80'sh2_0000_0000_0000_0000; c.sample();
    $display("%.2f %.2f", c.addr.get_coverage(), c.off.get_coverage());
    addr = '1; off = -80'sd7; c.sample();
    $display("%.2f %.2f", c.addr.get_coverage(), c.off.get_coverage());
    $display("%.2f %.2f", c.a4.get_coverage(), c.s3.get_coverage());
  end
endmodule
//> 33.33 33.33 31.25
//> 33.33 66.67
//> 66.67 100.00
//> Error: covergroup t.cg: illegal bin off.bad hit (value -7) at time 0
//> 100.00 100.00
//> 50.00 33.33
