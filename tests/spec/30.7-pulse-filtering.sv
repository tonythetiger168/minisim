`timescale 1ns/1ns
// IEEE 1800-2023 30.7, 30.7.1: pulse filtering at a module path output. A
// pulse narrower than the reject limit disappears, one narrower than the
// error limit becomes x for its width, a wider one passes. By default both
// limits are the delay (inertial: Figure 30-5). PATHPULSE$in$out sets the
// limits of one path declaration (named by its first input and output),
// and PATHPULSE$ those of every other path of the module.
//! icarus-skip: Icarus 13 does not filter path pulses
module buf75(input a, output y);
  assign y = a;
  specify
    (a => y) = (7, 9);
  endspecify
endmodule
module lim(input a, output y);
  assign y = a;
  specify
    (a => y) = 10;
    specparam PATHPULSE$a$y = (2, 6);
  endspecify
endmodule
module dff(input clk, data, clr, pre, output q);
  assign q = clk ^ data ^ clr ^ pre;
  specify
    (clk => q) = 12;
    (data => q) = 10;
    (clr, pre *> q) = 4;
    specparam
      PATHPULSE$clk$q = (2,9),
      PATHPULSE$clr$q = (0,4),
      PATHPULSE$ = 3;
  endspecify
endmodule
module top;
  reg a = 0, b = 0, clk = 0, data = 0, clr = 0, pre = 0;
  wire y1, y2, q;
  buf75 u1(a, y1);
  lim u2(b, y2);
  dff u3(clk, data, clr, pre, q);
  initial begin
    $monitor("%0t: y1=%b y2=%b q=%b", $time, y1, y2, q);
    // Figure 30-5: a 4-wide pulse; rise 7, fall 9, so the output pulse is 2
    // wide, under the fall's limits of 9: rejected. A 10-wide one passes.
    #20 a = 1; #4 a = 0;
    #20 a = 1; #10 a = 0;
    // A 0-pulse whose trailing edge is due first: the leading one is cancelled.
    #20 a = 1; #20 a = 0; #1 a = 1;
    // Limits 2 and 6 of a 10 delay: 1 rejected, 4 to x, 8 passes.
    #20 b = 1; #1 b = 0;
    #20 b = 1; #4 b = 0;
    #20 b = 1; #8 b = 0;
    // clk => q: limits 2, 9. Width 5 is x, 9 passes.
    #20 clk = 1; #5 clk = 0;
    #30 clk = 1; #9 clk = 0;
    // data => q: PATHPULSE$ gives 3, 3. Width 2 is rejected, 3 passes.
    #30 data = 1; #2 data = 0;
    #30 data = 1; #3 data = 0;
    // clr, pre => q: 0, 4 from PATHPULSE$clr$q. pre shares it: width 1 is x.
    #30 clr = 1; #1 clr = 0;
    #30 pre = 1; #1 pre = 0;
    #30 pre = 1; #4 pre = 0;
    #30 $finish;
  end
endmodule
//> 0: y1=x y2=x q=x
//> 4: y1=x y2=x q=0
//> 9: y1=0 y2=x q=0
//> 10: y1=0 y2=0 q=0
//> 51: y1=1 y2=0 q=0
//> 63: y1=0 y2=0 q=0
//> 81: y1=1 y2=0 q=0
//> 146: y1=1 y2=x q=0
//> 150: y1=1 y2=0 q=0
//> 170: y1=1 y2=1 q=0
//> 178: y1=1 y2=0 q=0
//> 200: y1=1 y2=0 q=x
//> 205: y1=1 y2=0 q=0
//> 235: y1=1 y2=0 q=1
//> 244: y1=1 y2=0 q=0
//> 304: y1=1 y2=0 q=1
//> 307: y1=1 y2=0 q=0
//> 331: y1=1 y2=0 q=x
//> 332: y1=1 y2=0 q=0
//> 362: y1=1 y2=0 q=x
//> 363: y1=1 y2=0 q=0
//> 393: y1=1 y2=0 q=1
//> 397: y1=1 y2=0 q=0
