// 23.3.3.1: an output port that the module also reads, on a net that
// something outside drives too, is coerced to inout: inside, the module
// sees the value the outside driver puts on the net.
module bcell(output [3:0] q, input oe, input ld);
  reg [3:0] r = 4'h3;
  assign q = oe ? r : 4'bz;
  always @(posedge ld) r = q;   // reads its own output port
endmodule
module top;
  wire [3:0] bus;
  reg [3:0] v;
  reg en, oe, ld;
  assign bus = en ? v : 4'bz;
  bcell c(bus, oe, ld);
  initial begin
    en = 1; oe = 0; ld = 0; v = 4'ha;
    #1 $display("in %h out %h", c.q, bus);
    ld = 1;
    #1 en = 0; oe = 1; ld = 0;
    #1 $display("in %h out %h", c.q, bus);
  end
endmodule
//> in a out a
//> in a out a
