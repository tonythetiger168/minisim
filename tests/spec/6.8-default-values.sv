// IEEE 1800-2023 6.8 (Table 6-7): with no initializer, int and bit start at 0,
// real at 0.0, string empty; a 4-state integer starts at x.
module t; int a; bit [3:0] b; real r; string s; integer i; initial $display("%0d %b %f [%s] %0d", a, b, r, s, i); endmodule

//> 0 0000 0.000000 [] x
