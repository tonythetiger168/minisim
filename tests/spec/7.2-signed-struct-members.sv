// IEEE 1800-2023 7.2, 11.8.1: a signed member of an unpacked struct (int,
// byte) reads as signed, in a struct variable, in a queue element and
// through a class handle, so s.f < 0 holds for a negative member.
//! icarus-skip: Icarus 13 does not support queues of structs with string members in classes
module top;
  typedef struct { string path; int offset; byte b; } sl_t;
  class H; sl_t slices[$]; endclass
  sl_t s;
  sl_t q[$];
  H h;
  initial begin
    s = '{"a", -1, -2};
    q.push_back(s);
    h = new;
    h.slices.push_back('{"b", -3, 4});
    $display("%0d %0d %0d %0d", s.offset, s.b, s.offset < 0, s.b < 0);
    $display("%0d %0d", q[0].offset, q[0].offset < 0);
    foreach (h.slices[j]) $display("%0d %0d %0d", h.slices[j].offset, h.slices[j].offset < 0, h.slices[j].b < 0);
  end
endmodule

//> -1 -2 1 1
//> -1 1
//> -3 1 0
