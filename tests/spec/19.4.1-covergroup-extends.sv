// IEEE 1800-2023 19.4.1: an embedded covergroup in a derived class that
// extends the base class's covergroup. It keeps the base's coverpoints,
// replaces the one it declares again under the same name, and adds its own.
//! icarus-skip: Icarus 13 has no covergroups
class base;
  bit [3:0] a, b;
  covergroup cg;
    ca: coverpoint a { bins lo = {[0:7]}; bins hi = {[8:15]}; }
    cb: coverpoint b { bins z = {0}; }
  endgroup
  function new();
    cg = new;
  endfunction
endclass
class derived extends base;
  covergroup extends cg;
    ca: coverpoint a { bins one = {1}; bins two = {2}; }
    cc: coverpoint a + b { bins s = {3}; }
  endgroup
  function new();
    super.new();
    cg = new;
  endfunction
endclass
module top;
  initial begin
    static derived d = new;
    static base b = new;
    d.a = 1; d.b = 2; d.cg.sample();
    b.a = 1; b.b = 2; b.cg.sample();
    $display("%0.2f %0.2f", d.cg.get_inst_coverage(), b.cg.get_inst_coverage());
  end
endmodule

//> 50.00 25.00
