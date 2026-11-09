// IEEE 1800-2023 10.3.3: A delay on a continuous assignment or net delays
// the update (10.3.3, 28.16).
//! xfail: continuous-assign and net delays are ignored
//! icarus-skip: Icarus 13 does not support net delays
module top;
  logic a = 0;
  wire w1, w2;
  wire #2 w3;
  assign #2 w1 = a;
  assign #(3, 1) w2 = a;
  assign w3 = a;
  initial begin
    #1 $display("%0t %b %b %b", $time, w1, w2, w3);
    #2 $display("%0t %b %b %b", $time, w1, w2, w3);
    a = 1;
    #1 $display("%0t %b %b %b", $time, w1, w2, w3);
    #2 $display("%0t %b %b %b", $time, w1, w2, w3);
    a = 0;
    #1 $display("%0t %b %b %b", $time, w1, w2, w3);
  end
endmodule

//> 1 x x x
//> 3 0 0 0
//> 4 0 0 0
//> 6 1 1 1
//> 7 1 0 1
