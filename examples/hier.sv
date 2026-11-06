// Parent instantiates child. x drives u.a, and u.b drives y.
module child(input logic a, output logic b);
  assign b = a;
endmodule

module parent(input logic x, output logic y);
  child u(.a(x), .b(y));
endmodule
