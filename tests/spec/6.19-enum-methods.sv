// IEEE 1800-2023 6.19: Enumeration methods first, last, next, prev, num,
// name (6.19.5).
//! xfail: enum methods are an error
//! icarus-skip: Icarus 13 does not support e.first().name()
module top;
  typedef enum {A, B, C} e_t;
  e_t e;
  initial begin
    e = A;
    $display("%s %0d", e.name(), e.num());
    e = e.next();
    $display("%s", e.name());
    e = e.next(2);
    $display("%s", e.name());
    e = e.prev();
    $display("%s %s %s", e.name(), e.first().name(), e.last().name());
  end
endmodule

//> A 3
//> B
//> A
//> C A C
