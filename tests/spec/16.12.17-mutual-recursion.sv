// IEEE 1800-2023 16.12.17: mutually recursive properties. ph1 and ph2 hand
// the check to each other one tick later. q3 and q4 pass their arguments
// on to each other, and q4 adds accept_on and reject_on around q3, so an
// attempt that is inside q4 when bb is 1 passes and one inside q4 when
// abort is 1 fails.
//! icarus-skip: Icarus has no recursive properties
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic s1 = 0, s2 = 0, ok1 = 1, ok2 = 1;
  logic p = 1, b = 0, ab = 0;

  property ph1; s1 |-> (ok1 and (1'b1 |=> ph2)); endproperty
  property ph2; s2 |-> (ok2 and (1'b1 |=> ph1)); endproperty
  a1: assert property (@(posedge clk) ph1) else $display("%0t a1 fail", $time);

  property q3(x, bit bb, abort);
    x and (1'b1 |=> q4(x, bb, abort));
  endproperty
  property q4(x, bit bb, abort);
    accept_on(bb) reject_on(abort) q3(x, bb, abort);
  endproperty
  a2: assert property (@(posedge clk) q3(p, b, ab)) else $display("%0t a2 fail", $time);

  initial begin
    @(negedge clk) s1 = 1;                              // seen at 15: ph2 is due at 25
    @(negedge clk) begin s1 = 0; s2 = 1; ok2 = 0; b = 1; end   // 25: a1 fails; a2's attempts so far pass
    @(negedge clk) begin s2 = 0; ok2 = 1; b = 0; end
    @(negedge clk) p = 0;                               // 45: the attempts of 25, 35 and 45 fail
    @(negedge clk) p = 1;
    @(negedge clk);
    @(negedge clk) ab = 1;                              // 75: the attempts of 55 and 65 fail
    @(negedge clk) ab = 0;
    #30 $finish;
  end
endmodule
//> 25 a1 fail
//> 45 a2 fail
//> 45 a2 fail
//> 45 a2 fail
//> 75 a2 fail
//> 75 a2 fail
