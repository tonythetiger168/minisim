// IEEE 1800-2023 19.6.1.2: a cross with filter (`binsof(...) with (...)`)
// over a coverpoint wider than 64 bits. The filter sees every value tuple
// of the bins it selects, values above 2^64 included.
//! icarus-skip: Icarus 13 does not support covergroups
module t;
  bit [79:0] a;
  bit [3:0] b;
  localparam bit [79:0] B = 80'h1_0000_0000_0000_0000;   // 2^64
  covergroup cg;
    ca: coverpoint a { bins lo = {[B : B + 3]}; bins hi = {[B + 8 : B + 9]}; }
    cb: coverpoint b { bins v[] = {[0:3]}; }
    x: cross ca, cb {
      // a is B + 8 + b for some value in the bins: (hi, v[0]) and (hi, v[1])
      bins diag = binsof(ca) with (ca - B == cb + 8);
      ignore_bins low = binsof(ca.lo);   // left: diag, (hi, v[2]), (hi, v[3])
    }
  endgroup
  cg c = new;
  initial begin
    $display("%0d", c.x.get_inst_coverage() == 0);
    a = B + 9; b = 0; c.sample();   // (hi, v[0]): diag, though 9 != 8 + 0
    $display("%.2f", c.x.get_coverage());
    a = B + 8; b = 3; c.sample();   // (hi, v[3])
    $display("%.2f", c.x.get_coverage());
    a = B + 1; b = 1; c.sample();   // ignored
    $display("%.2f", c.x.get_coverage());
  end
endmodule
//> 1
//> 33.33
//> 66.67
//> 66.67
