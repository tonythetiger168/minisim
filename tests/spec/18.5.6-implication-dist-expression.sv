// 18.5.6, 18.5.3: an implication written as an expression whose right side
// is a dist, g -> (v dist {...}), also chained (a -> (b -> (v dist ...))):
// the distribution applies only while the guards hold.
//! icarus-skip: Icarus does not support constraints
class C;
  rand bit [3:0] a[4];
  bit gateA, gateB;
  constraint c {
    foreach (a[i]) gateA -> (gateB -> (a[i] dist { 4'd0 := 3, [4'd1 : 4'd4] := 1 }));
  }
endclass
module top;
  C c = new;
  int outside, zeros, big;
  initial begin
    c.gateA = 1; c.gateB = 1;
    repeat (100) begin
      void'(c.randomize());
      foreach (c.a[i]) begin
        if (c.a[i] > 4) outside++;
        if (c.a[i] == 0) zeros++;
      end
    end
    c.gateB = 0;
    repeat (100) begin
      void'(c.randomize());
      foreach (c.a[i]) if (c.a[i] > 4) big++;
    end
    $display("outside %0d, zeros more than a quarter %0d, free values above 4 %0d", outside, zeros > 100, big > 0);
  end
endmodule
//> outside 0, zeros more than a quarter 1, free values above 4 1
