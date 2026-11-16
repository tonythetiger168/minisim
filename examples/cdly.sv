// Delayed continuous assignments (10.3.3) in msim -compile: a single delay,
// rise/fall delays on a part of a net, a delay expression read at each change,
// and the x every delayed driver holds until its first value arrives.
module cdly;
  reg clk = 0, a = 0;
  reg [7:0] b = 0;
  integer d = 3;
  wire #2 w1 = a;
  wire [15:0] w2;
  assign #(3, 5) w2[11:4] = b;
  wire #(d) w3 = ~a;
  assign #1 w4 = a & b[0];
  wire w4;
  always #4 clk = ~clk;
  always @(posedge clk) begin a <= ~a; b <= b + 3; if (b == 9) d = 1; end
  always @(w1 or w2 or w3 or w4) $display("%0t w1=%b w2=%h w3=%b w4=%b", $time, w1, w2, w3, w4);
  initial #80 $finish;
endmodule
