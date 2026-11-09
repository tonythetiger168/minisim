// IEEE 1800-2023 16.10: local variables in a property: a value assigned by a
// match item when the antecedent matches and checked ticks later, with
// overlapping attempts that each keep their own value; ++ in a repetition.
//! icarus-skip: Icarus 13 does not support assertion local variables
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic valid = 0;
  initial $assertvacuousoff;
  int din = 0, dout = 0;
  int pipe[4];
  int bump = 0;
  always @(posedge clk) begin
    dout <= pipe[2] + 1 + bump;
    pipe[2] <= pipe[1]; pipe[1] <= pipe[0]; pipe[0] <= din;
  end
  // 16.10: the value captured at the start is checked later
  property p_pipe;
    int x;
    (valid, x = din) |-> ##4 (dout == x + 1);
  endproperty
  a_pipe: assert property (@(posedge clk) p_pipe)
    $display("%0t pass", $time); else $display("%0t FAIL", $time);
  // counting with ++ in a repetition
  logic a = 0, b = 0;
  int cnt_exp = 0;
  property p_cnt;
    int n;
    (a, n = 0) ##1 (b, n++)[*1:$] ##1 (!b && n == cnt_exp);
  endproperty
  a_cnt: cover property (@(posedge clk) p_cnt) $display("%0t cover cnt", $time);
  initial begin
    @(negedge clk) valid = 1; din = 10;
    @(negedge clk) valid = 1; din = 20;
    @(negedge clk) valid = 0; din = 99;
    @(negedge clk) din = 5;
    @(negedge clk) valid = 1; din = 1;
    @(negedge clk) valid = 0;
    @(negedge clk);
    @(negedge clk) bump = 1;
    @(negedge clk) bump = 0;
    repeat (5) @(negedge clk);
    a = 1; cnt_exp = 3;
    @(negedge clk) a = 0; b = 1;
    @(negedge clk);
    @(negedge clk);
    @(negedge clk) b = 0;
    @(negedge clk);
    $finish;
  end
endmodule

//> 55 pass
//> 65 pass
//> 95 FAIL
//> 185 cover cnt
