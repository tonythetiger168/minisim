// IEEE 1800-2023 28.6: Three-state gates bufif0, bufif1, notif0, notif1
// (28.6).
//! xfail: bufif/notif are errors
module top;
  logic d = 1, e = 1;
  wire o1, o2, o3, o4;
  bufif1 b1(o1, d, e);
  bufif0 b0(o2, d, e);
  notif1 n1(o3, d, e);
  notif0 n0(o4, d, e);
  initial begin
    #1 $display("%b %b %b %b", o1, o2, o3, o4);
    e = 0; #1 $display("%b %b %b %b", o1, o2, o3, o4);
    e = 1'bx; #1 $display("%b %b", o1, o3);
  end
endmodule

//> 1 z 0 z
//> z 1 z 0
//> x x
