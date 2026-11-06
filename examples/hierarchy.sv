// A parent instantiates children and values cross the ports.
// NAMED / POS / OTHER: named and positional connections, two instances of inv.
// EXPR: an input can be an expression, not only a signal name.
// HIDE: the child's own signal, printed from inside the child.
// GRAND: the same connection one level deeper.
// PROC0 / PROC1: a procedural write in the child is visible on the parent.
// XZ: X and Z cross a port.
// W8 / W4: a parameter override selects the elaborated port width.
// INOUT: an inout net is driven from inside the child.
module leaf(input logic [3:0] d, output logic [3:0] q);
  assign q = d;
endmodule

module mid(input logic [3:0] d, output logic [3:0] q);
  leaf u(.d(d), .q(q));
endmodule

module inv(input logic a, output logic b);
  assign b = ~a;
endmodule

module buf4(input logic [3:0] a, output logic [3:0] b);
  logic [3:0] hidden;
  assign hidden = a;
  assign b = hidden;
  initial #1 $display("HIDE %b", hidden);
endmodule

module hold(output logic q);
  initial begin
    q = 1'b0;
    #1 q = 1'b1;
  end
endmodule

module wide #(parameter W = 4) (input logic [W-1:0] a, output logic [W-1:0] b);
  assign b = a;
endmodule

module tie(inout wire p);
  assign p = 1'b1;
endmodule

module hierarchy;
  logic x;
  logic y, y2, y3, yn;
  logic qt;
  logic [3:0] d, g, xz, yz;
  logic [7:0] a8, b8;
  logic [3:0] a4, b4;
  wire io;

  inv u(.a(x), .b(y));
  inv v(.a(1'b0), .b(y2));
  inv p(x, y3);
  inv n(.a(~x), .b(yn));
  mid m(.d(d), .q(g));
  buf4 z(.a(xz), .b(yz));
  hold h(.q(qt));
  wide #(.W(8)) w8(.a(a8), .b(b8));
  wide #(.W(4)) w4(.a(a4), .b(b4));
  tie t(io);

  initial begin
    x = 1'b1;
    d = 4'b1100;
    xz = 4'b1x0z;
    a8 = 8'b10110001;
    a4 = 4'b0110;
    $strobe("PROC0 %b", qt);
    #2;
    $display("NAMED %b", y);
    $display("OTHER %b", y2);
    $display("POS %b", y3);
    $display("EXPR %b", yn);
    $display("GRAND %b", g);
    $display("XZ %b", yz);
    $display("PROC1 %b", qt);
    $display("W8 %b", b8);
    $display("W4 %b", b4);
    $display("INOUT %b", io);
    $finish;
  end
endmodule
