// IEEE 1800-2023 19.5.1.1: single with bins over every value of a
// coverpoint wider than 64 bits (2^100 values here), and a wildcard bin
// with a with clause: a sampled value is in the bin when the with
// expression is true for it. Also on a 64-bit coverpoint, where a with bin
// over [0:$] has 2^64 candidate values, and a wildcard with bin on a narrow
// one.
//! icarus-skip: Icarus 13 does not support covergroups
module t;
  bit [99:0] v;
  bit [63:0] u;
  bit [7:0] n;
  covergroup cg;
    cv: coverpoint v {
      bins mul3 = {[0:$]} with (item % 3 == 0);
      bins big = cv with (item > 1000);
      wildcard bins hi_even = {100'h8_????_????_????_????_????_????} with (item[0] == 0);
    }
    cu: coverpoint u {
      bins odd = {[0:$]} with (item[0]);
    }
    cn: coverpoint n {
      wildcard bins lo_seven = {8'b????_?111} with (item > 100);
    }
  endgroup
  cg c = new;
  task automatic hit(bit [99:0] x, bit [63:0] y, bit [7:0] z);
    v = x; u = y; n = z; c.sample();
    $display("%.2f %.2f %.2f", c.cv.get_coverage(), c.cu.get_coverage(), c.cn.get_coverage());
  endtask
  initial begin
    hit(7, 2, 7);                          // in no bin: 7 is not over 100
    hit(9, 64'hffff_ffff_ffff_fffe, 103);  // mul3; u even; 103 = 8'b0110_0111
    hit(100'h8 << 96 | 2, 64'hffff_ffff_ffff_ffff, 15);   // big, hi_even; u odd
  end
endmodule
//> 0.00 0.00 0.00
//> 33.33 0.00 100.00
//> 100.00 100.00 100.00
