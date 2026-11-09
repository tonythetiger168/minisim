// IEEE 1800-2023 8.14: A subclass member with the same name hides the base
// member. Through a base-class handle the base members (non-virtual) are used.
module top;
  class Packet;
    integer i = 1;
    function integer get(); get = i; endfunction
  endclass
  class LinkedPacket extends Packet;
    integer i = 2;
    function integer get(); get = -i; endfunction
  endclass
  LinkedPacket lp;
  Packet p;
  initial begin
    lp = new;
    p = lp;
    $display("%0d %0d %0d %0d", p.i, p.get(), lp.i, lp.get());
  end
endmodule

//> 1 1 2 -2
