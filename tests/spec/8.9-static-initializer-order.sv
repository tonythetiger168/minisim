// IEEE 1800-2023 8.9, 6.8, 10.5: a static property starts at its default
// (null, 0) before any static initializer runs, so an initializer that calls
// a lazy singleton getter of another class (or its own class) sees null and
// not an unknown value.
//! icarus-skip: Icarus 13 does not support static class handles with initializers
module top;
  class TypeId #(type T = int);
    typedef TypeId #(T) this_type;
    static this_type my_type = get_type();
    static int made;
    static function this_type get_type();
      if (my_type == null) begin my_type = new(); made++; end
      return my_type;
    endfunction
  endclass
  class Registry;
    static TypeId #(byte) id = TypeId #(byte)::get_type();
    static bit registered = register();
    static function bit register(); return id != null; endfunction
  endclass
  initial begin
    $display("%0d %0d", TypeId #(int)::made, TypeId #(byte)::made);
    $display("%0d %0d", TypeId #(int)::get_type() == TypeId #(int)::my_type, Registry::registered);
  end
endmodule

//> 1 1
//> 1 1
