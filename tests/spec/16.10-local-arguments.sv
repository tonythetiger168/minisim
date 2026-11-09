// IEEE 1800-2023 16.8.2, 16.10: a local input argument and a declaration
// initializer; local variables assigned on both sides of intersect; match
// items on first_match.
//! icarus-skip: Icarus 13 does not support assertion local variables
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic req = 0, ack = 0, start = 0, a = 0, b = 0, c = 0;
  int id = 0, aid = 0, cur = 0, d = 0, e = 0, out = 0;
  initial $assertvacuousoff;
  // a local input argument (16.8.2) and an initializer
  sequence s_rd(local input int tag);
    int seen = 100;
    (req && id == tag, seen = id) ##[1:3] (ack && aid == seen);
  endsequence
  a_rd: assert property (@(posedge clk) start |-> s_rd(cur))
    $display("%0t rd pass", $time); else $display("%0t rd FAIL", $time);
  // intersect: both sides assign, the result sees both (16.10)
  property p_int;
    int x, y;
    (((a, x = d) ##1 b) intersect (c ##1 (1, y = e))) ##1 (out == x + y);
  endproperty
  a_int: assert property (@(posedge clk) a |-> p_int)
    $display("%0t int pass", $time); else $display("%0t int FAIL out=%0d", $time, out);
  // first_match with a match item
  property p_fm;
    int t;
    first_match((b ##[1:2] c), t = d) |=> (out == t);
  endproperty
  a_fm: assert property (@(posedge clk) p_fm)
    $display("%0t fm pass", $time); else $display("%0t fm FAIL", $time);
  initial begin
    @(negedge clk) start = 1; cur = 7; req = 1; id = 7;           // t15
    @(negedge clk) start = 0; req = 0; id = 0;                     // t25
    @(negedge clk) ack = 1; aid = 7;                              // t35: pass
    @(negedge clk) ack = 0; start = 1; cur = 3; req = 1; id = 3;  // t45
    @(negedge clk) start = 0; req = 0;                             // t55
    @(negedge clk) ack = 1; aid = 4;                              // t65 wrong tag
    @(negedge clk) ack = 0;                                        // t75
    @(negedge clk);                                                // t85: fail (window over at 75)
    @(negedge clk) a = 1; c = 1; d = 2;                           // t95
    @(negedge clk) a = 0; c = 0; b = 1; e = 5; d = 0;             // t105
    @(negedge clk) b = 0; out = 7;                                 // t115: pass
    @(negedge clk) a = 1; c = 1; d = 1;                           // t125
    @(negedge clk) a = 0; c = 0; b = 1; e = 1;                    // t135
    @(negedge clk) b = 0; out = 7;                                 // t145: fail (1+1)
    @(negedge clk) b = 1; d = 4;                                   // t155
    @(negedge clk) b = 0; c = 1; d = 9;                            // t165: c: fm match, t=9
    @(negedge clk) c = 0; out = 9;                                 // t175: fm pass
    @(negedge clk) $finish;
  end
endmodule

//> 35 rd pass
//> 75 rd FAIL
//> 115 int pass
//> 135 fm FAIL
//> 145 int FAIL out=7
//> 175 fm pass
