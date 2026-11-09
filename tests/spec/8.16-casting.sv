// IEEE 1800-2023 8.16: $cast assigns a base handle to a subclass handle only
// when the object really is of that subclass; the task form reports an
// error on failure, the function form returns 0.
module top;
  class Base; int b = 1; endclass
  class Sub extends Base; int s = 2; endclass
  class Other extends Base; endclass
  Base h;
  Sub s;
  Other o;
  int ok1, ok2, ok3;
  initial begin
    h = Sub::new;
    ok1 = $cast(s, h);
    ok2 = $cast(o, h);
    h = null;
    ok3 = $cast(s, h);
    $display("%0d %0d %0d %0d", ok1, ok2, ok3, s == null);
  end
endmodule

//> 1 0 1 1
