// IEEE 1800-2023 8.7: Constructors. new allocates the object and runs its
// constructor; default argument values apply; properties get their
// declared initial values before the body runs.
module top;
  class Packet;
    int command = 5;
    int addr;
    bit [7:0] data = 8'hA5;
    function new(int a = 16);
      addr = a + command;
    endfunction
  endclass
  class Plain;
    int x = 7;
    logic [3:0] l;
  endclass
  Packet p, q;
  Plain r;
  initial begin
    p = new;
    q = new(1);
    r = new();
    $display("%0d %0d %h %0d %0d %b", p.addr, q.addr, p.data, r.x, q.command, r.l);
  end
endmodule

//> 21 6 a5 7 5 xxxx
