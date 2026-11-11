// Fairness: a strong obligation in an assume limits the loops a liveness
// counterexample may use.
module fair(input logic clk, rst, req0, req1, a, b, c, output logic gnt0, gnt1);
  assign gnt0 = req0;
  assign gnt1 = req1 & ~req0;   // fixed priority: 1 starves while 0 keeps asking
  m_hold1: assume property (@(posedge clk) disable iff (rst) req1 && !gnt1 |=> req1);
  // requester 0 always lets go again
  m_fair0: assume property (@(posedge clk) disable iff (rst) req0 |-> s_eventually !req0);
  // proven only because of m_fair0
  a_live1: assert property (@(posedge clk) disable iff (rst) req1 |-> s_eventually gnt1);
  // fails: nobody has to ask, and that loop is fair
  a_some: assert property (@(posedge clk) disable iff (rst) s_eventually gnt0);
  // fails: after one grant requester 1 may stop asking
  a_two: assert property (@(posedge clk) disable iff (rst) req1 |-> s_eventually (gnt1 ##1 gnt1));

  m_a: assume property (@(posedge clk) disable iff (rst) s_eventually a);
  m_c: assume property (@(posedge clk) disable iff (rst) c |=> s_eventually (c ##1 !c ##1 !c));
  a_y: assert property (@(posedge clk) disable iff (rst) s_eventually a);                          // proven by m_a
  a_x: assert property (@(posedge clk) disable iff (rst) s_eventually (a && b));                   // fails: b is free
  a_z: assert property (@(posedge clk) disable iff (rst) s_eventually (a ##1 a));                  // fails: a may alternate
  a_c: assert property (@(posedge clk) disable iff (rst) c |-> s_eventually !c);                   // proven by m_c
  a_cc: assert property (@(posedge clk) disable iff (rst) c |-> s_eventually (!c ##1 !c ##1 !c));  // fails
endmodule
