// msim -vcd-class: class objects in the waveform (tests/msim_vcd_class.cmake).
class Base;
  bit [3:0] kind;
endclass
class Pkt extends Base;
  bit [7:0] addr;
  logic [15:0] data;
  string tag;          // left out of the waveform
  Pkt next;
  function new(bit [7:0] a, logic [15:0] d);
    addr = a; data = d; kind = 4'h3;
  endfunction
endclass
module tb;
  Pkt p;
  initial begin
    #5 p = new(8'h11, 16'hbeef);
    #5 p.addr = 8'h22;
    #5 p.next = new(8'h33, 16'h1234);
    #5 p = p.next;
    #5 p = null;
    #5 $finish;
  end
endmodule
