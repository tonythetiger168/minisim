// IEEE 1800-2023 18.5.8.1 / 18.5.6: a queue's size decided by another
// random variable through implications in both directions. Sizes are solved
// before the elements, but not necessarily before the scalars that decide
// them, so every call succeeds and the size agrees with op.
//! icarus-skip: Icarus 13 has no constraints
module top;
  class t;
    rand bit [2:0] op;
    rand bit [7:0] q[$];
    constraint c_q { op == 2 -> q.size() inside {[2:6]}; op != 2 -> q.size() == 0; }
  endclass
  initial begin
    static t x = new;
    int fails = 0, bad = 0, two = 0;
    repeat (200) begin
      if (!x.randomize()) fails++;
      else if (x.op == 2 ? (x.q.size() < 2 || x.q.size() > 6) : x.q.size() != 0) bad++;
      if (x.op == 2) two++;
    end
    $display("fails=%0d bad=%0d op2 seen=%0d", fails, bad, two > 0);
  end
endmodule
//> fails=0 bad=0 op2 seen=1
