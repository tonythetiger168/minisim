// IEEE 1800-2023 23.3.3.7: a plain internal net joined through an inout port
// to a tri1 / tri0 net is one net of the external kind (table 23-1). Its
// drivers on both sides resolve together and the pull applies when all are z.
//! icarus-skip: Icarus has no interfaces
interface bus_if;
  logic o, oe;
  tri1  m;
  assign m = oe ? o : 1'bz;
endinterface
module dev(inout logic m);
  logic drive = 0, dout = 1;
  assign m = drive ? dout : 1'bz;
  wire seen = (m !== 1'b0);
endmodule
module top;
  bus_if v();
  dev d(.m(v.m));
  logic o0, oe0;
  tri0 p;
  assign p = oe0 ? o0 : 1'bz;
  dev e(.m(p));
  initial begin
    v.oe = 0; v.o = 1; oe0 = 0; o0 = 1;
    #1 $display("released: %b %b seen=%b", v.m, p, d.seen);
    v.oe = 1; v.o = 0; oe0 = 1;
    #1 $display("outside drives: %b %b seen=%b", v.m, p, d.seen);
    v.oe = 0; oe0 = 0; d.drive = 1; d.dout = 0; e.drive = 1; e.dout = 1;
    #1 $display("inside drives: %b %b", v.m, p);
    v.oe = 1; v.o = 1;
    #1 $display("conflict: %b", v.m);
    v.oe = 0; d.drive = 0; e.drive = 0;
    #1 $display("released: %b %b", v.m, p);
  end
endmodule
//> released: 1 0 seen=1
//> outside drives: 0 1 seen=0
//> inside drives: 0 1
//> conflict: x
//> released: 1 0
