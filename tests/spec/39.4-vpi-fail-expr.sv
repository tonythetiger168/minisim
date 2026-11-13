// IEEE 1800-2023 39.4.2: on cbAssertionFailure, info->detail.failExpr is
// the expression that failed: an immediate assertion's condition, or the
// boolean of a single-expression property, or with sequence operators the
// boolean that did not hold in the failing step (c in b |-> ##1 c).
//! icarus-skip: Icarus 13 has no assertion API
//! args: data/vpi/failexpr.c
module top;
  logic clk = 0, a = 1, b = 0, c = 0;
  always #5 clk = ~clk;
  p_simple: assert property (@(posedge clk) a && !b);
  p_seq: assert property (@(posedge clk) b |-> ##1 c);
  initial begin
    $watch;
    #12 b = 1;                    // p_simple fails at 15; p_seq at 25
    #10 b = 0;
    begin : blk
      im: assert (c == 1);        // fails at 22
    end
    #20 $finish;
  end
endmodule
//> p_simple failed at 15: vpiOperation a && !b
//> Error: "39.4-vpi-fail-expr.sv", 10: top.p_simple: at time 15
//> im failed at 22: vpiOperation c == 1
//> Error: "39.4-vpi-fail-expr.sv", 17: top.blk.im: at time 22
//> p_seq failed at 25: vpiReg c
//> Error: "39.4-vpi-fail-expr.sv", 11: top.p_seq: at time 25
