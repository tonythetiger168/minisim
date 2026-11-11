// Liveness corner cases: what holds, what fails with a loop, and what the
// engine says it does not decide.
module live2(input logic clk, rst);
  logic [1:0] m; logic t;
  always_ff @(posedge clk) if (rst) begin m <= 0; t <= 0; end else begin m <= m == 2 ? 2'd0 : m + 1'b1; t <= ~t; end
  wire a110 = m != 2;   // 1 1 0 1 1 0 ...
  // hold
  h_snext:  assert property (@(posedge clk) disable iff (rst) s_nexttime (t || !t));
  h_asnext: assert property (@(posedge clk) disable iff (rst) always s_nexttime (t || !t));
  h_gf2:    assert property (@(posedge clk) disable iff (rst) (always s_eventually t) and (always s_eventually !t));
  h_renew:  assert property (@(posedge clk) disable iff (rst) always nexttime s_eventually (a110 ##1 a110));
  h_or:     assert property (@(posedge clk) disable iff (rst) always ((s_eventually (t && !t)) or nexttime (a110 || !a110)));
  h_until:  assert property (@(posedge clk) disable iff (rst) 1 s_until t);
  h_ev3:    assert property (@(posedge clk) disable iff (rst) s_eventually (a110 ##1 a110 ##1 !a110));
  h_impl:   assert property (@(posedge clk) disable iff (rst) !a110 |=> s_eventually (!a110 ##1 a110));
  // fail
  f_three:  assert property (@(posedge clk) disable iff (rst) s_eventually (a110 ##1 a110 ##1 a110));
  f_ab:     assert property (@(posedge clk) disable iff (rst) s_eventually (1 ##1 (t && !t)));
  f_until:  assert property (@(posedge clk) disable iff (rst) a110 s_until (t && !t));
  f_until2: assert property (@(posedge clk) disable iff (rst) 1 s_until (t && !t));
  f_gf:     assert property (@(posedge clk) disable iff (rst) always s_eventually (t ##1 t));
  f_strong: assert property (@(posedge clk) disable iff (rst) strong(1[*] ##1 (t && !t)));
  // not decided: say so
  u_fg:     assert property (@(posedge clk) disable iff (rst) s_eventually always t);
  u_fg2:    assert property (@(posedge clk) disable iff (rst) s_eventually always (t || !t));
endmodule
