// IEEE 1800-2023 18.4: the elements of a rand dynamic array or queue are
// random, also when each element is an unpacked struct; its rand members
// are solved with the size, and constraints name them through the element.
//! icarus-skip: Icarus 13 has no constraints
typedef struct { rand int value; rand bit [3:0] tag; } entry_t;
class C;
  rand entry_t vs[];
  rand entry_t q[$];
  constraint cs { vs.size() == 3; q.size() inside {[1:4]}; }
  constraint cv { vs[0].value == 0; vs[1].value == 31; vs[2].value == -15;
                  foreach (vs[i]) vs[i].tag == i + 1; }
  constraint cq { foreach (q[i]) { q[i].value inside {[-5:5]}; q[i].tag != 0; } }
endclass
module top;
  initial begin
    static C c = new;
    automatic int ok = 1, sizes = 0, vals = 0;
    repeat (40) begin
      if (!c.randomize()) ok = 0;
      if (c.vs.size() != 3 || c.vs[0].value != 0 || c.vs[1].value != 31 || c.vs[2].value != -15) ok = 0;
      foreach (c.vs[i]) if (c.vs[i].tag != i + 1) ok = 0;
      foreach (c.q[i]) begin
        if (c.q[i].value < -5 || c.q[i].value > 5 || c.q[i].tag == 0) ok = 0;
        vals |= 1 << (c.q[i].value + 5);
      end
      sizes |= 1 << c.q.size();
    end
    $display("ok=%0d sizes=%b values-vary=%0d", ok, sizes[4:0], $countones(vals) > 5);
  end
endmodule
//> ok=1 sizes=11110 values-vary=1
