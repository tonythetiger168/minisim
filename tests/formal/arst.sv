// Asynchronous set / reset and latches.
module arst(input logic clk, input logic rst, input logic aclr_n, input logic aset,
            input logic en, input logic le, input logic [3:0] d);
  logic [3:0] q, lq, cnt;
  logic       f;
  // an asynchronous clear next to the (synchronous) reset
  always_ff @(posedge clk or negedge aclr_n)
    if (!aclr_n) q <= '0;
    else if (rst) q <= '0;
    else if (en) q <= d;
  // set and clear, clear first
  always_ff @(posedge clk or negedge aclr_n or posedge aset)
    if (!aclr_n) f <= 1'b0;
    else if (aset) f <= 1'b1;
    else if (rst) f <= 1'b0;
    else f <= f ^ en;
  // a counter the clear restarts at once
  always_ff @(posedge clk or negedge aclr_n)
    if (!aclr_n) cnt <= 4'd0;
    else if (rst) cnt <= 4'd0;
    else if (cnt != 4'd9) cnt <= cnt + 4'd1;
  always_latch if (le) lq = d;

  // the clear acts in the same cycle
  a_clr:      assert property (@(posedge clk) disable iff (rst) !aclr_n |-> q == 0 && !f && cnt == 0);
  a_set:      assert property (@(posedge clk) disable iff (rst) aclr_n && aset |-> f);
  a_cnt:      assert property (@(posedge clk) disable iff (rst) cnt <= 4'd9);
  // wrong for an asynchronous clear: the load can be undone before the next edge
  a_load_bad: assert property (@(posedge clk) disable iff (rst) aclr_n && en && d == 4'd5 |=> q == 4'd5);
  a_load:     assert property (@(posedge clk) disable iff (rst) aclr_n && en && d == 4'd5 ##1 aclr_n |-> q == 4'd5);
  // the latch holds while it is closed and follows while it is open
  a_hold:     assert property (@(posedge clk) disable iff (rst) !le |-> lq == $past(lq));
  a_open:     assert property (@(posedge clk) disable iff (rst) le |-> lq == d);
  a_hold_bad: assert property (@(posedge clk) disable iff (rst) lq == $past(lq));
endmodule
