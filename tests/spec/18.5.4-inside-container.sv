// IEEE 1800-2023 18.5.4 / 11.4.13: a set membership constraint whose set is a
// queue or dynamic array of the caller (randomize() with) or of the class:
// every solution is an element, and every element can be picked.
//! icarus-skip: Icarus 13 does not support constraints
module top;
  class Flit;
    rand bit [51:0] payload;
    rand bit [3:0] credit;
    rand bit err;
    bit [3:0] allowed[];
    constraint c_err { err dist {0 := 9, 1 := 1}; }
    constraint c_cr { credit inside {allowed}; }
  endclass
  Flit f;
  int ok = 1, bad, seen[4];
  initial begin
    bit [51:0] corners[$] = '{52'h0, {52{1'b1}}, 52'h1, 52'hA_AAAA_AAAA_AAAA};
    f = new;
    f.allowed = '{3, 9};
    repeat (200) begin
      ok &= f.randomize() with { payload inside {corners}; err == 0; };
      if (!(f.payload inside {corners}) || !(f.credit inside {3, 9}) || f.err) bad++;
      foreach (corners[i]) if (f.payload == corners[i]) seen[i]++;
    end
    $display("%0d %0d %0d", ok, bad, seen[0] > 0 && seen[1] > 0 && seen[2] > 0 && seen[3] > 0);
  end
endmodule

//> 1 0 1
