// IEEE 1800-2023 21.2.1.5 Strength format: %v prints a net's strength as a
// three-character mnemonic (We1, Pu0, St1, Su1, HiZ) or, for a mix of
// strengths, the 0 and 1 strength digits followed by the value.
module t;
  wire a, b, c, d, e;
  tri0 f;
  supply1 vdd;
  wire g;
  logic r = 1;
  assign (strong0, weak1) a = 1;
  assign (pull0, pull1) b = 0;
  assign (strong0, strong1) c = 1; assign (strong0, strong1) c = 0;
  assign (pull0, weak1) d = 1'bx;
  pullup(e);
  initial #1 $display("%v %v %v %v %v %v %v %v %v", a, b, c, d, e, f, vdd, g, r);
endmodule
//> We1 Pu0 StX 53X Pu1 Pu0 Su1 HiZ St1
