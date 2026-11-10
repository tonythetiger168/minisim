// IEEE 1800-2023 23.10: defparam overrides a parameter of an instance
// (23.10.1).
module m #(parameter P = 1) ();
  initial $display("%0d", P);
endmodule
module top;
  m u();
  defparam u.P = 5;
endmodule

//> 5
