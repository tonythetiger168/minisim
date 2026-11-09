// IEEE 1800-2023 8.13: A subclass inherits the properties and methods of its
// base class and can add its own.
module top;
  class Packet;
    int addr = 1;
    function int get_addr(); return addr; endfunction
  endclass
  class LinkedPacket extends Packet;
    LinkedPacket next;
    function LinkedPacket get_next(); return next; endfunction
  endclass
  LinkedPacket l1, l2;
  initial begin
    l1 = new; l2 = new;
    l1.next = l2;
    l2.addr = 7;
    $display("%0d %0d %0d", l1.get_addr(), l1.get_next().get_addr(), l2.get_next() == null);
  end
endmodule

//> 1 7 1
