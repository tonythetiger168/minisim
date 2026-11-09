// IEEE 1800-2023 8.24: extern methods are prototyped in the class and
// defined outside it with Class::name.
module top;
  class Packet;
    int v;
    extern function new(int x);
    extern function int get();
    extern virtual task bump(int n);
    extern static function int sq(int x);
  endclass
  function Packet::new(int x); v = x; endfunction
  function int Packet::get(); return v; endfunction
  task Packet::bump(int n); #1 v += n; endtask
  function int Packet::sq(int x); return x * x; endfunction
  Packet p;
  initial begin
    p = new(3);
    p.bump(4);
    $display("%0t %0d %0d", $time, p.get(), Packet::sq(5));
  end
endmodule

//> 1 7 25
