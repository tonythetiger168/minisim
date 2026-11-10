// FSM and condition coverage self-test (tests/msim_fsm.cmake).
//   ctrl: two-process FSM on an enum, asynchronous reset
//   one:  one-process FSM on localparams, synchronous reset, conditions
//   ifs:  an FSM written with if (state == ...), and a counter that is not an FSM
// +define+MORE drives the error path too.
module ctrl(input logic clk, rst_n, go, done, err, output logic busy);
  typedef enum logic [1:0] {IDLE, RUN, WAIT, FAIL} st_t;
  st_t state, next;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) state <= IDLE;
    else state <= next;
  always_comb begin
    next = state;
    case (state)
      IDLE: if (go) next = RUN;
      RUN:  if (err) next = FAIL;
            else if (done) next = WAIT;
      WAIT: next = IDLE;
      FAIL: next = IDLE;
    endcase
  end
  assign busy = (state == RUN) || (state == WAIT);
endmodule
module one(input logic clk, rst, a, b, c, output logic [1:0] q);
  localparam [1:0] S0 = 2'd0, S1 = 2'd1, S2 = 2'd2;
  reg [1:0] st;
  always @(posedge clk) begin
    if (rst) st <= S0;
    else case (st)
      S0: if (a && b) st <= S1;
      S1: st <= (c || a) ? S2 : S0;
      S2: if (a && b && c) st <= S0;
      default: st <= S0;
    endcase
  end
  assign q = st;
endmodule
module ifs(input logic clk, rst, t, output logic o);
  logic [1:0] ph;
  logic [2:0] cnt;
  always_ff @(posedge clk) begin
    cnt <= cnt + 3'd1;                 // arithmetic: not an FSM
    if (rst) begin ph <= 2'd0; cnt <= '0; end
    else if (ph == 2'd0) begin if (t) ph <= 2'd1; end
    else if (ph == 2'd1) ph <= t ? 2'd2 : 2'd0;
    else ph <= 2'd0;
  end
  assign o = ph != 2'd0 && cnt[0];
endmodule
module tb;
  logic clk = 0, rst_n = 0, go = 0, done = 0, err = 0, a = 0, b = 0, c = 0;
  logic busy; logic [1:0] q;
  ctrl u(.*);
  one  v(.clk, .rst(!rst_n), .a, .b, .c, .q);
  logic o;
  ifs  w(.clk, .rst(!rst_n), .t(a), .o);
  always #5 clk = ~clk;
  initial begin
    #12 rst_n = 1;
    @(negedge clk) go = 1; a = 1; b = 1;
    @(negedge clk) go = 0; done = 1; b = 0;
    @(negedge clk) done = 0;
    repeat (3) @(negedge clk);
`ifdef MORE
    go = 1; err = 1; c = 1; b = 1;
    repeat (4) @(negedge clk);
`endif
    $display("busy=%b q=%0d", busy, q);
    $finish;
  end
endmodule
