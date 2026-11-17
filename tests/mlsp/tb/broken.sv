module broken;
  logic [3:0] q;
  assign q = undeclared_sig;
  nosuch_mod u0 ();
endmodule
