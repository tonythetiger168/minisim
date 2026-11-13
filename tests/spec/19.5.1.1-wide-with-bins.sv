// IEEE 1800-2023 19.5.1.1: a with clause on the bins of a coverpoint wider
// than 64 bits keeps the listed values for which the expression is true.
// A single bin, a name[] array (one bin per value kept), a name[N] array
// and ignore_bins with a with clause.
//! icarus-skip: Icarus 13 does not support covergroups
module t;
  bit [79:0] v;
  localparam bit [79:0] B = 80'h1_0000_0000_0000_0000;   // 2^64
  covergroup cg;
    coverpoint v {
      bins odd = {[B : B + 9]} with (item % 2 == 1);
      bins tri_[] = {[B + 20 : B + 29]} with ((item - B) % 3 == 0);
      bins four[2] = {[B + 40 : B + 47]} with (item[0] == 0);
      ignore_bins ig = {[B + 1 : B + 3]} with (item != B + 2);
    }
  endgroup
  cg c = new;
  task automatic hit(bit [79:0] x);
    v = x; c.sample();
    $display("%.2f", c.get_coverage());
  endtask
  // 6 bins: odd {5,7,9 after the ignored 1,3}, tri_[] {21,24,27},
  // four[0] {40,42}, four[1] {44,46} (all above 2^64).
  initial begin
    hit(B + 3);    // ignored
    hit(B + 5);    // odd
    hit(B + 4);    // in no bin
    hit(B + 21);   // tri_[...21]
    hit(B + 22);   // in no bin
    hit(B + 46);   // four[1]
    hit(B + 24);   // tri_[...24]
  end
endmodule
//> 0.00
//> 16.67
//> 16.67
//> 33.33
//> 33.33
//> 50.00
//> 66.67
