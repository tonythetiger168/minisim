// IEEE 1800-2023 6.24.2: $cast to a string, an unpacked struct or an array.
// The cast is valid whenever the types are cast compatible, except that a
// fixed-size array takes a container only of the same size (7.6); a failed
// cast leaves the destination unchanged and returns 0.
//! icarus-skip: Icarus 13 does not support $cast or unpacked structs
//> 1 hi
//> 1 '{a:3, b:4}
//> 1 c d
//> 1 '{5, 6, 7}
//> 1 '{5, 6, 7}
//> 1 '{9, 8, 7}
//> 0 '{9, 8, 7}
//> 1 66
//> 1 B
//> Error: $cast failed: the source has the wrong number of elements
module top;
  typedef struct { int a; byte b; } pair_t;
  typedef struct packed { logic [3:0] hi, lo; } nib_t;
  string s;
  pair_t x, y;
  nib_t n;
  int fixed3[3];
  int dyn[];
  int q[$];
  int two[$] = '{1, 2};
  logic [7:0] v;
  int ok;
  initial begin
    ok = $cast(s, "hi");            $display("%0d %s", ok, s);
    x = '{3, 4};
    ok = $cast(y, x);               $display("%0d %p", ok, y);
    ok = $cast(n, 8'hcd);           $display("%0d %h %h", ok, n.hi, n.lo);
    fixed3 = '{5, 6, 7};
    ok = $cast(dyn, fixed3);        $display("%0d %p", ok, dyn);
    ok = $cast(q, fixed3);          $display("%0d %p", ok, q);
    q = '{9, 8, 7};
    ok = $cast(fixed3, q);          $display("%0d %p", ok, fixed3);
    ok = $cast(fixed3, two);        $display("%0d %p", ok, fixed3);
    ok = $cast(v, "B");             $display("%0d %0d", ok, v);
    ok = $cast(s, v);               $display("%0d %s", ok, s);
    $cast(fixed3, two);
  end
endmodule
