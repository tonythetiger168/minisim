// One-hot FSMs and short-circuit conditions (tests/msim_fsm.cmake).
//   oh:   case (1'b1) on state[K], next-state logic that clears and sets a bit
//   ohif: if (s[K]) on a one-hot register
//   tb:   conditions with a function call, a system function, and (+define+CLS) class handles
module oh(input logic clk, rst_n, go, stop, output logic act);
  localparam int IDLE = 0, RUN = 1, DONE = 2;
  logic [2:0] state, next;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin state <= '0; state[IDLE] <= 1'b1; end
    else state <= next;
  always_comb begin
    next = '0;
    unique case (1'b1)
      state[IDLE]: if (go) next[RUN] = 1'b1; else next[IDLE] = 1'b1;
      state[RUN]:  if (stop) next[DONE] = 1'b1; else next[RUN] = 1'b1;
      state[DONE]: next[IDLE] = 1'b1;
      default:     next[IDLE] = 1'b1;
    endcase
  end
  assign act = state[RUN];
endmodule
module ohif(input logic clk, rst, t);
  logic [1:0] s;
  always @(posedge clk)
    if (rst) s <= 2'b01;
    else if (s[0]) begin if (t) s <= 2'b10; end
    else if (s[1]) s <= 2'b01;
endmodule
`ifdef CLS
class pkt; int len; bit ok; function bit good(); return ok; endfunction endclass
`endif
module tb;
  logic clk = 0, rst_n = 0, go = 0, stop = 0, act;
  oh u(.*);
  ohif v(.clk, .rst(!rst_n), .t(go));
  always #5 clk = ~clk;
  function bit odd(input int x); return x[0]; endfunction
  logic [3:0] vec = 4'b0110;
  int n = 0, hits = 0;
`ifdef CLS
  pkt p;
`endif
  initial begin
    #12 rst_n = 1;
    @(negedge clk) go = 1;
    @(negedge clk) go = 0;
    repeat (2) @(negedge clk);
`ifdef CLS
    if (p != null && p.len > 2) hits++;
    p = new; p.len = 5;
    if (p != null && p.len > 2) hits++;
    if (go || p.good()) hits++;
`endif
    for (n = 0; n < 4; n++) if (n > 1 && odd(n)) hits++;
    for (n = 0; n < 3; n++) begin vec = vec << 1; if (stop || $countones(vec) > 1) hits++; end
    $display("hits=%0d act=%b", hits, act);
    $finish;
  end
endmodule
