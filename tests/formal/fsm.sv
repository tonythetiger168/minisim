// mformal test: an FSM with always_comb, functions, immediate assertions, an assumption
// and sequence operators. tests/mformal.cmake has the expected verdicts.
module fsm(input logic clk, input logic rst_n, input logic req, input logic done, output logic busy, output logic gnt);
  typedef enum logic [1:0] {IDLE, REQ, RUN, FIN} st_t;
  st_t st, nx;
  function automatic logic is_active(st_t s); return s == REQ || s == RUN; endfunction
  function logic [1:0] enc(input logic a, input logic b); enc = {a, b}; endfunction
  always_comb begin
    nx = st; gnt = 1'b0;
    unique case (st)
      IDLE: if (req) nx = REQ;
      REQ: begin gnt = 1'b1; nx = RUN; end
      RUN: if (done) nx = FIN;
      FIN: nx = IDLE;
    endcase
  end
  always_comb busy = (st != IDLE);
  logic [1:0] e;
  always_comb e = enc(busy, gnt);
  always_ff @(posedge clk or negedge rst_n) if (!rst_n) st <= IDLE; else st <= nx;
  always_comb begin
    if (rst_n) begin
      a_imm: assert (!(gnt && st == IDLE));
      a_imm_bad: assert (!(e == 2'b10));
    end
  end
  a_gnt_once: assert property (@(posedge clk) disable iff (!rst_n) gnt |=> !gnt);
  a_gnt_run: assert property (@(posedge clk) disable iff (!rst_n) gnt |-> ##1 st == RUN);
  a_live: assert property (@(posedge clk) disable iff (!rst_n) req && st == IDLE |-> s_eventually st == FIN);
  a_fin: assert property (@(posedge clk) disable iff (!rst_n) st == FIN |-> ##1 st == IDLE ##0 !busy);
  a_until: assert property (@(posedge clk) disable iff (!rst_n) st == RUN |-> busy until_with st != RUN);
  a_no_skip: assert property (@(posedge clk) disable iff (!rst_n) st == IDLE |=> st != RUN);
  m_done: assume property (@(posedge clk) st == RUN |-> ##[0:3] done);
  a_bounded: assert property (@(posedge clk) disable iff (!rst_n) st == REQ |-> ##[1:5] st == FIN);
  a_bounded_bad: assert property (@(posedge clk) disable iff (!rst_n) st == REQ |-> ##[1:3] st == FIN);
  c_fin: cover property (@(posedge clk) st == FIN);
endmodule
