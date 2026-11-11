// A sequence's end point: s.triggered (16.13.6).
module trig(input logic clk, input logic rst, input logic req, input logic gnt);
  logic busy, done;
  logic [1:0] n;
  always_ff @(posedge clk)
    if (rst) begin busy <= 0; done <= 0; n <= 0; end
    else begin
      done <= busy && gnt;
      if (busy && gnt) begin busy <= 0; n <= n + 2'd1; end
      else if (req) busy <= 1;
    end
  sequence s_hs; busy ##1 done; endsequence          // a handshake completed
  sequence s_start; !busy && req ##1 busy; endsequence
  // when a handshake has just completed, the design is idle again
  a_idle: assert property (@(posedge clk) disable iff (rst) s_hs.triggered |-> !busy);
  // wrong: a start does not mean it was idle the cycle after
  a_bad: assert property (@(posedge clk) disable iff (rst) s_start.triggered |=> !busy);
  // used as a plain boolean inside a sequence
  a_seq: assert property (@(posedge clk) disable iff (rst) done |-> s_hs.triggered);
  a_seq_bad: assert property (@(posedge clk) disable iff (rst) busy |-> ##[1:2] s_hs.triggered);
  c_two: cover property (@(posedge clk) disable iff (rst) s_hs.triggered ##[1:$] s_hs.triggered);
  c_never: cover property (@(posedge clk) disable iff (rst) s_hs.triggered && s_start.triggered);
endmodule
