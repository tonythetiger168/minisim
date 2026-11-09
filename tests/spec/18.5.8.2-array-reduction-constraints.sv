// IEEE 1800-2023 18.5.8.2: array reduction methods in constraints: sum()
// of a rand dynamic array, sum() with (...) to widen each element, and a
// constraint on a non-rand queue's contents.
//! icarus-skip: Icarus 13 does not support classes
class Split;
  rand bit [7:0] part[];
  rand int unsigned total;
  int weights[$] = '{1, 2, 3};
  rand int pick;
  constraint c_n { part.size() == 4; total == 600; }
  constraint c_sum { part.sum() with (int'(item)) == total; }
  constraint c_each { foreach (part[i]) part[i] > 0; }
  constraint c_pick { pick == weights.sum() with (item * 2); }
endclass

module top;
  Split s = new;
  int bad, acc;
  initial begin
    repeat (20) begin
      if (!s.randomize()) bad++;
      acc = 0;
      foreach (s.part[i]) begin acc += s.part[i]; if (s.part[i] == 0) bad++; end
      if (acc != 600 || s.part.size() != 4) bad++;
      if (s.pick != 12) bad++;
    end
    $display("bad %0d", bad);
  end
endmodule

//> bad 0
