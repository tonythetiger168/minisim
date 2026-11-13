// IEEE 1800-2023 16.12.17: a named property may instantiate itself, after a
// time advance, with the same arguments. Each attempt then unfolds one level
// per clock tick: p_always(p) acts as always p, p_until(p, q) as p until q.
//! icarus-skip: Icarus has no concurrent assertions
module t;
  bit clk, first, a, b, req, ack;
  always #5 clk = !clk;
  property p_always(p);
    p and (1'b1 |=> p_always(p));
  endproperty
  property p_until(p, q);
    q or (p and (1'b1 |=> p_until(p, q)));
  endproperty
  property p_noack;
    !ack and (1'b1 |=> p_noack);
  endproperty
  a1: assert property (@(posedge clk) first |-> p_always(a)) else $display("%0t a low", $time);
  a2: assert property (@(posedge clk) req |-> p_until(req, ack)) else $display("%0t req dropped", $time);
  a3: assert property (@(posedge clk) b |-> p_noack) else $display("%0t ack after b", $time);
  initial begin
    a = 1; first = 1;
    @(negedge clk) req = 1; first = 0;
    @(negedge clk) ;
    @(negedge clk) ack = 1;              // ends the request of 15
    @(negedge clk) req = 0; ack = 0;
    @(negedge clk) a = 0; req = 1;       // a1 fails at 55
    @(negedge clk) a = 1; req = 0;       // the request of 55 drops at 65
    @(negedge clk) b = 1;
    @(negedge clk) b = 0;
    @(negedge clk) ack = 1;              // a3 fails at 95
    @(negedge clk) ack = 0;
    #10 $finish;
  end
endmodule
//> 55 a low
//> 65 req dropped
//> 95 ack after b
