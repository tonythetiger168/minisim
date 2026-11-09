// IEEE 1800-2023 8.30: std::weak_reference refers to an object without
// keeping it alive. get() returns the referent until clear() is called;
// get_id() gives a stable nonzero id per object (0 for null), the same
// through any handle type.
//! icarus-skip: Icarus 13 does not support std::weak_reference
module top;
  class Obj; int v; endclass
  class ExObj extends Obj; endclass
  typedef weak_reference #(Obj) obj_ref;
  typedef weak_reference #(ExObj) ex_obj_ref;
  initial begin
    automatic ExObj my_obj = new;
    automatic Obj other = new;
    automatic obj_ref r = new(my_obj);
    my_obj.v = 5;
    $display("%0d %0d", r.get().v, r.get() == my_obj);
    $display("%0d %0d %0d", obj_ref::get_id(my_obj) == ex_obj_ref::get_id(my_obj),
             obj_ref::get_id(my_obj) != obj_ref::get_id(other), obj_ref::get_id(null));
    $display("%0d", obj_ref::get_id(my_obj) != 0);
    r.clear();
    $display("%0d", r.get() == null);
  end
endmodule

//> 5 1
//> 1 1 0
//> 1
//> 1
