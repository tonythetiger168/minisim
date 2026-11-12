// IEEE 1800-2023 18.11 with 18.4.2: a randc property named in
// randomize(v) keeps its cycle: each permitted value comes once before any
// comes again.
//! icarus-skip: Icarus 13 does not support randomize()
class W;
  randc bit [3:0] s;
  int n = 3;
  constraint c { s < n; }
endclass
module t;
  initial begin
    W w = new; int seen[3]; bit ok = 1;
    repeat (5) begin
      foreach (seen[i]) seen[i] = 0;
      repeat (3) begin void'(w.randomize(s)); seen[w.s]++; end
      foreach (seen[i]) if (seen[i] != 1) ok = 0;
    end
    $display("cyclic %0d", ok);
  end
endmodule
//> cyclic 1
