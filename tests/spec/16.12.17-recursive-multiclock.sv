// IEEE 1800-2023 16.12.17 with 16.13: a recursive property in a multiclock
// assertion. `hand` checks req on c1; the next c2 tick must see ack, and
// one c2 tick after that the property starts again on the next c1 tick.
// So while req keeps coming, ack must hold at every c2 tick.
//! icarus-skip: Icarus has no recursive properties
module t;
  bit c1, c2, req, ack;
  always #5 c1 = !c1;
  always #7 c2 = !c2;
  property hand;
    @(posedge c1) req |=> @(posedge c2) (ack and (1'b1 |=> hand));
  endproperty
  a_h: assert property (hand) else $display("%0t hand fail", $time);
  initial begin
    #3 req = 1; ack = 1;
    #40 req = 0;          // the chains end: no more checks
    #20 ack = 0;
    #20 req = 1;          // 83: the attempt at 85 sees ack low at 91
    #20 ack = 1;          // 103
    #20 ack = 0;          // 123: every chain running (from the attempts at
                          // 95, 105, 115 and 125) fails at the next c2 tick, 133
    #10 ack = 1;
    #20 $finish;
  end
endmodule
//> 91 hand fail
//> 133 hand fail
//> 133 hand fail
//> 133 hand fail
//> 133 hand fail
