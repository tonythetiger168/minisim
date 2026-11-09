// IEEE 1800-2023 19.5: explicit bins: single and array bins, [N] fixed
// arrays, $ bounds, default bins (not counted, 19.11), and the 19.11 counts.
//! icarus-skip: Icarus 13 does not support covergroups
module top;
  bit [9:0] v_a;
  covergroup cg;
    coverpoint v_a {
      bins a   = { [0:63], 65 };
      bins b[] = { [127:150], [148:191] };   // 65 bins
      bins c[] = { 200, 201, 202 };
      bins d   = { [1000:$] };
      bins fixed[4] = { [1:10], 1, 4, 7 };
      bins others[] = default;
    }
  endgroup
  cg g = new;
  int cov, tot;
  initial begin
    v_a = 5;    g.sample();   // a, fixed[1]
    v_a = 130;  g.sample();   // b[130]
    v_a = 201;  g.sample();   // c[201]
    v_a = 1023; g.sample();   // d
    v_a = 500;  g.sample();   // default
    $display("%0.4f", g.get_coverage());
    void'(g.get_inst_coverage(cov, tot));
    $display("%0d of %0d", cov, tot);
  end
endmodule

//> 6.7568
//> 5 of 74
