// IEEE 1800-2023 7.8.5: an associative array may be indexed by an unpacked
// struct (or other user-defined type); keys are equal when all members are
// equal, and first / next / foreach / delete walk them in a fixed order.
//! icarus-skip: Icarus 13 has no struct keys
module top;
  typedef struct { byte b; int i; } k_t;
  string m[k_t];
  k_t k, j;
  initial begin
    k = '{2, 20}; m[k] = "two";
    k = '{1, 10}; m[k] = "one";
    k = '{1, 5};  m[k] = "half";
    $display("%0d %0d", m.num(), m.exists('{2, 20}));
    if (m.first(j)) do $display("%0d %0d %s", j.b, j.i, m[j]); while (m.next(j));
    foreach (m[x]) $display("fe %0d %0d", x.b, x.i);
    m.delete('{1, 10});
    $display("%0d", m.num());
  end
endmodule
//> 3 1
//> 1 5 half
//> 1 10 one
//> 2 20 two
//> fe 1 5
//> fe 1 10
//> fe 2 20
//> 2
