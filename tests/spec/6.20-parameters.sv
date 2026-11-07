// IEEE 1800-2023 6.20 Constants: parameter and localparam values, including
// one computed from another and one overridden at instantiation.
module child #(parameter int W = 4) ();
  localparam int W2 = W * 2;
  initial $display("W=%0d W2=%0d", W, W2);
endmodule
module t;
  parameter int N = 3;
  localparam logic [7:0] MASK = (1 << N) - 1;
  child #(.W(5)) u ();
  initial $display("N=%0d MASK=%b", N, MASK);
endmodule
//> W=5 W2=10
//> N=3 MASK=00000111
