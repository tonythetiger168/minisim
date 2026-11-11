// SVA local variables (16.10): a two-stage pipeline with a valid bit.
module local_(input logic clk, input logic rst, input logic req, input logic [3:0] d);
  logic [3:0] q1, q2;
  logic       v1, v2;
  always_ff @(posedge clk)
    if (rst) begin v1 <= 0; v2 <= 0; q1 <= 0; q2 <= 0; end
    else begin v1 <= req; v2 <= v1; q1 <= d; q2 <= q1 + (q1 == 4'd13); end   // 13 comes out as 14

  // the data that went in comes out two cycles later ... except 13
  property p_data; logic [3:0] x; (req, x = d) |-> ##2 (v2 && q2 == x); endproperty
  a_data: assert property (@(posedge clk) disable iff (rst) p_data);
  property p_data_ok; logic [3:0] x; (req && d != 4'd13, x = d) |-> ##2 (v2 && q2 == x); endproperty
  a_data_ok: assert property (@(posedge clk) disable iff (rst) p_data_ok);
  // assigned in the middle of the antecedent, read on the way and at the end
  property p_mid; logic [3:0] x; (req ##1 (v1, x = q1) ##1 v2) |-> (q2 == x + (x == 4'd13)); endproperty
  a_mid: assert property (@(posedge clk) disable iff (rst) p_mid);
  // an initializer in the declaration, and two variables
  property p_init; logic [3:0] x = d; logic [3:0] y; (req ##1 (1, y = q1)) |-> (y == x) ##1 (q2 == y + (x == 4'd13)); endproperty
  a_init: assert property (@(posedge clk) disable iff (rst) p_init);
  property p_init_bad; logic [3:0] x = d; req |=> ##1 (q2 == x); endproperty
  a_init_bad: assert property (@(posedge clk) disable iff (rst) p_init_bad);
  // covers: some value comes out changed, no value below 13 does
  sequence s_chg(lim); logic [3:0] x; (req && d < lim, x = d) ##2 (q2 != x); endsequence
  c_changed: cover property (@(posedge clk) disable iff (rst) s_chg(4'd15));
  c_never:   cover property (@(posedge clk) disable iff (rst) s_chg(4'd13));
  // not followed: reported as skipped, with the reason
  property p_twice; logic [3:0] x; (req, x = d) ##1 (1, x = x + 1) |-> ##1 (q2 + 1 == x); endproperty
  a_twice: assert property (@(posedge clk) disable iff (rst) p_twice);
  property p_pos; logic [3:0] x; (req, x = d) |-> s_eventually (1, x = q2) ##0 (x == q2); endproperty
  a_pos: assert property (@(posedge clk) disable iff (rst) p_pos);
endmodule
