// mformal test: liveness. tests/mformal.cmake has the expected verdicts. An arbiter that can starve one side, and a fair one.
module live(input logic clk, input logic rst, input logic [1:0] req, output logic [1:0] gnt_prio, output logic [1:0] gnt_rr);
  // fixed priority: req[1] waits while req[0] keeps asking
  always_ff @(posedge clk) if (rst) gnt_prio <= 2'b00; else gnt_prio <= req[0] ? 2'b01 : req[1] ? 2'b10 : 2'b00;
  // round robin: the side not served last goes first
  logic last;
  always_ff @(posedge clk)
    if (rst) begin gnt_rr <= 2'b00; last <= 1'b0; end
    else if (req == 2'b11) begin gnt_rr <= last ? 2'b01 : 2'b10; last <= ~last; end
    else begin gnt_rr <= req; if (req != 2'b00) last <= req[1]; end
  default clocking @(posedge clk); endclocking
  default disable iff (rst);
  m_hold0: assume property (req[0] && !gnt_rr[0] |=> req[0]);
  m_hold1: assume property (req[1] && !gnt_rr[1] |=> req[1]);
  a_prio0: assert property (req[0] |-> s_eventually gnt_prio[0]);   // true
  a_prio1: assert property (req[1] |-> s_eventually gnt_prio[1]);   // false: starved
  a_rr0: assert property (req[0] |-> s_eventually gnt_rr[0]);       // true
  a_rr1: assert property (req[1] |-> s_eventually gnt_rr[1]);       // true
  a_rr1_fast: assert property (req[1] |-> ##[1:2] gnt_rr[1]);       // false: a request may be dropped once it was granted
  a_until: assert property (req[1] |-> req[1] s_until_with gnt_rr[1]);   // true
  a_always_ev: assert property (s_eventually (!req[0] || gnt_prio[0]));  // true
  a_idle: assert property (always s_eventually req == 2'b00);       // false: the inputs never have to stop
endmodule
