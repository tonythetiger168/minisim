// IEEE 1800-2023 8.16: $cast called as a function returns 0 on a failed cast
// and reports nothing, also when the result is thrown away with void'(...).
// Only the task form reports an error.
//! icarus-skip: Icarus 13 does not support $cast on class handles
module top;
  class Base; endclass
  class Sub extends Base; endclass
  class Other extends Base; endclass
  typedef enum {A, B} e_t;
  Base h;
  Sub s;
  e_t e;
  initial begin
    h = Other::new;
    void'($cast(s, h));
    void'($cast(e, 7));
    $display("%0d %s", s == null, e.name());
    h = Sub::new;
    void'($cast(s, h));
    $display("%0d", s == null);
  end
endmodule

//> 1 A
//> 0
