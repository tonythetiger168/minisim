// SystemVerilog parent of VHDL entities with unconstrained ports
// (uncon_ports.vhd): the parent gives each such port's index range as the
// parameters <port>__L and <port>__R (docs/VHDL.md).
module mixed_uncon;
  logic [5:0] v = 6'b101100;
  logic p;
  int n;
  logic [3:0] y;
  par #(.d__L(5), .d__R(0)) u (.d(v), .p(p), .n(n));
  inv #(.a__L(3), .a__R(0), .y__L(3), .y__R(0)) ui (.a(v[3:0]), .y(y));
  initial begin
    #1 $display("p=%b n=%0d y=%b", p, n, y);
    v = 6'b000001;
    #1 $display("p=%b y=%b", p, y);
  end
endmodule
