// IEEE 1800-2023 6.6.4: A trireg net starts at x and holds its last driven
// value when its drivers go to z (6.6.4, 28.15).
//! icarus-skip: Icarus 13 does not support trireg
module top;
  logic en = 0, d = 1;
  trireg t;
  assign t = en ? d : 1'bz;
  initial begin
    #1 $display("%b", t);
    en = 1; #1 $display("%b", t);
    en = 0; #1 $display("%b", t);
    d = 0; en = 1; #1 en = 0; #1 $display("%b", t);
  end
endmodule

//> x
//> 1
//> 1
//> 0
