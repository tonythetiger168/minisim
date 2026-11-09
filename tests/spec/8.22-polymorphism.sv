// IEEE 1800-2023 8.22: Polymorphism. An array of base-class handles holds
// objects of different subclasses; a virtual call runs each object's own
// method.
//! icarus-skip: Icarus 13 does not support pure virtual methods
module top;
  virtual class BasePacket;
    pure virtual function string name();
    virtual function int cost(); return 1; endfunction
  endclass
  class EtherPacket extends BasePacket;
    virtual function string name(); return "ether"; endfunction
    virtual function int cost(); return 3; endfunction
  endclass
  class TokenPacket extends BasePacket;
    virtual function string name(); return "token"; endfunction
  endclass
  class GigaPacket extends EtherPacket;
    virtual function string name(); return "giga"; endfunction
    virtual function int cost(); return super.cost() * 10; endfunction
  endclass
  BasePacket packets[$];
  int total;
  initial begin
    automatic EtherPacket e = new; automatic TokenPacket t = new; automatic GigaPacket g = new;
    packets = '{e, t, g};
    foreach (packets[i]) begin
      $display("%s %0d", packets[i].name(), packets[i].cost());
      total += packets[i].cost();
    end
    $display("total %0d", total);
  end
endmodule

//> ether 3
//> token 1
//> giga 30
//> total 34
