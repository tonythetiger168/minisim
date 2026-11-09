// IEEE 1800-2023 25.9: through a virtual interface: a task and a function
// of the interface, a clocking block's event, inputs and synchronous drives
// (14), a virtual interface kept in a parameterized class (the way a
// testbench passes it around), and one taken from an interface port.
//! icarus-skip: Icarus 13 does not support virtual interfaces
interface ifc(input logic clk);
  logic [3:0] x, y;
  function int twice(int a); return 2 * a; endfunction
  task automatic tick(int n); repeat (n) @(posedge clk); endtask
  clocking cb @(posedge clk);
    output x;
    input y;
  endclocking
endinterface

class Store #(type T = int);
  static T tab[string];
  static function void set(string k, T v); tab[k] = v; endfunction
  static function bit get(string k, output T v);
    if (!tab.exists(k)) return 0;
    v = tab[k];
    return 1;
  endfunction
endclass

module sub(ifc p);
  virtual ifc vp;
  initial begin vp = p; #1 vp.y = 4'h7; end
endmodule

module top;
  logic clk = 0;
  always #5 clk = ~clk;
  ifc a(clk), c(clk);
  sub s(c);
  virtual ifc v;
  initial begin
    Store#(virtual ifc)::set("vif", a);
    if (!Store#(virtual ifc)::get("vif", v)) $display("missing");
    $display("%0d", v.twice(21));
    a.y = 4'h9;
    @(v.cb);
    v.cb.x <= 4'h5;
    $display("%h at %0t", v.cb.y, $time);
    @(v.cb);
    #1 $display("%h", a.x);
    v.tick(2);
    $display("%0t %h", $time, s.vp.y);
    $finish;
  end
endmodule

//> 42
//> 9 at 5
//> 5
//> 35 7
