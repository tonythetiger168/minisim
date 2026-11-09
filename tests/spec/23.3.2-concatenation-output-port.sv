// IEEE 1800-2023 23.3.2 Module instance port connections: an output port
// connected to a concatenation drives each part with its slice.
module src(output logic [7:0] q);
  assign q = 8'hA5;
endmodule
module t;
  logic [4:0] hi;
  logic [2:0] lo;
  logic [7:0] w [2];
  logic flag;
  src u0(.q({hi, lo}));
  src u1(.q({w[1][6:0], flag}));
  initial #1 $display("%b %b %b %b", hi, lo, w[1][6:0], flag);
endmodule
//> 10100 101 1010010 1
