//! args: -only fsm -top fsm
// A clean machine: every state entered and left, a default, a reset.
module fsm_ok(input logic clk, rst_n, go, done, output logic busy);
  typedef enum logic [1:0] {IDLE, RUN, WAIT} st_t;
  st_t st, nx;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) st <= IDLE;
    else st <= nx;
  always_comb begin
    nx = st;
    case (st)
      IDLE: if (go) nx = RUN;
      RUN: nx = WAIT;
      WAIT: if (done) nx = IDLE;
      default: nx = IDLE;
    endcase
  end
  assign busy = st != IDLE;
endmodule

// LOST is never entered, STUCK is never left, and the case has no default.
module fsm_bad(input logic clk, rst_n, go, output logic [1:0] o);
  localparam logic [2:0] IDLE = 3'd0, RUN = 3'd1, STUCK = 3'd2, LOST = 3'd3;
  logic [2:0] st;                                 //~ FSM001 FSM002 FSM003
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) st <= IDLE;
    else
      case (st)
        IDLE: if (go) st <= RUN;
        RUN: st <= STUCK;
        STUCK: st <= STUCK;
        LOST: st <= IDLE;
      endcase
  assign o = st[1:0];
endmodule

// No reset: the machine starts anywhere. Every encoding is a state, so the
// missing default is not a finding.
module fsm_norst(input logic clk, t, output logic o);
  logic st;                                       //~ FSM004
  always_ff @(posedge clk)
    case (st)
      1'b0: if (t) st <= 1'b1;
      1'b1: if (t) st <= 1'b0;
    endcase
  assign o = st;
endmodule

module fsm(input logic clk, rst_n, go, done, t, output logic busy, o2, output logic [1:0] o);
  fsm_ok u_ok(.*);
  fsm_bad u_bad(.*);
  fsm_bad u_bad2(.clk, .rst_n, .go(done), .o());
  fsm_norst u_n(.clk, .t, .o(o2));
endmodule
