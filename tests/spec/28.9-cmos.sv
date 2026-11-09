// IEEE 1800-2023 28.9: A cmos switch is an nmos and a pmos in parallel
// (28.9).
module top;
  logic d = 1, n = 1, p = 0;
  wire o;
  cmos c(o, d, n, p);
  initial begin
    #1 $display("%b", o);
    n = 0; p = 1; #1 $display("%b", o);
  end
endmodule

//> 1
//> z
