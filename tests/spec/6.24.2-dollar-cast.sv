// IEEE 1800-2023 6.24.2: $cast checks an enum value at run time; as a task
// it is an error to fail (6.24.2).
//! icarus-skip: Icarus 13 does not support $cast
module top;
  typedef enum {A, B, C} e_t;
  e_t e;
  int ok;
  initial begin
    ok = $cast(e, 1);
    $display("%0d %s", ok, e.name());
    ok = $cast(e, 5);
    $display("%0d %s", ok, e.name());
  end
endmodule

//> 1 B
//> 0 B
