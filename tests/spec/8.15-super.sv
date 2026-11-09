// IEEE 1800-2023 8.15: super reaches the base class members that the
// subclass overrides, including the base constructor.
module top;
  class Packet;
    integer value;
    function new(integer v); value = v; endfunction
    function integer delay(); delay = value * value; endfunction
  endclass
  class LinkedPacket extends Packet;
    integer value;
    function new(); super.new(3); value = 5; endfunction
    function integer delay(); delay = super.delay() + value * super.value; endfunction
  endclass
  LinkedPacket p;
  initial begin
    p = new;
    $display("%0d", p.delay());
  end
endmodule

//> 24
