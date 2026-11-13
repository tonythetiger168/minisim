// IEEE 1800-2023 19.6.1.2: cross bins chosen with a with filter. A tuple of
// bins is selected when the filter is true for at least `matches` of its
// value tuples (1 by default; `matches $` means all of them). The cross's own
// name selects every tuple.
//! icarus-skip: Icarus 13 has no covergroups
module top;
  bit [3:0] a, b;
  covergroup cg;
    ca: coverpoint a { bins lo = {[0:3]}; bins hi = {[4:7]}; }
    cb: coverpoint b { bins lo = {[0:3]}; bins hi = {[4:7]}; }
    x: cross ca, cb {
      bins eq = x with (ca == cb);
      bins big = x with (ca + cb > 2) matches $;
      bins lt = binsof(ca.lo) with (ca < cb) matches 3;
    }
    y: cross ca, cb { bins eq = y with (ca == cb); }
  endgroup
  cg c = new;
  initial begin
    a = 1; b = 1; c.sample();
    $display("%0.2f %0.2f", c.x.get_inst_coverage(), c.y.get_inst_coverage());
    a = 5; b = 2; c.sample();
    $display("%0.2f %0.2f", c.x.get_inst_coverage(), c.y.get_inst_coverage());
  end
endmodule

//> 66.67 33.33
//> 100.00 66.67
