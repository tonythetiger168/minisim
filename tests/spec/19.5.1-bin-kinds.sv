// IEEE 1800-2023 19.5.1, 19.5.4-19.5.6, 19.11.1: auto bins with auto_bin_max
// and ignore_bins, enum auto bins, signed $ bounds, illegal_bins, wildcard,
// and bins with a with clause.
//! icarus-skip: Icarus 13 does not support covergroups
module top;
  typedef enum {RED, GREEN, BLUE} color_t;
  color_t col;
  bit [2:0] b;
  bit signed [2:0] s;
  bit [3:0] w;
  covergroup ct;
    cb: coverpoint b { option.auto_bin_max = 4; ignore_bins ig = {[0:1], [5:6]}; }   // the 19.11.1 example
    cc: coverpoint col;                                  // auto[RED], auto[GREEN], auto[BLUE]
    cs: coverpoint s { bins neg = {[$:-1]}; bins pos = {[1:$]}; illegal_bins zero = {0}; }
    cw: coverpoint w { wildcard bins hi = {4'b11??}; bins mod3[] = {[0:11]} with (item % 3 == 0); }
  endgroup
  ct g = new;
  initial begin
    b = 2; col = BLUE; s = -3; w = 13; g.sample();
    b = 7; col = RED;  s = 2;  w = 6;  g.sample();
    s = 0; g.sample();   // illegal
    $display("cb %0.2f cc %0.2f cs %0.2f cw %0.2f", g.cb.get_inst_coverage(), g.cc.get_inst_coverage(),
             g.cs.get_inst_coverage(), g.cw.get_inst_coverage());
    $display("all %0.2f", g.get_inst_coverage());
  end
endmodule

//> Error: covergroup top.ct: illegal bin cs.zero hit (value 0) at time 0
//> cb 66.67 cc 66.67 cs 100.00 cw 40.00
//> all 68.33
