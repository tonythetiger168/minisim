// IEEE 1800-2023 16.16: an assertion with no clock of its own takes the
// leading clock written in the sequence or property it names: a sequence
// with a subroutine call item under cover property 16.11, a clocked
// sequence as an implication antecedent, and a clocked property inside not.
//! icarus-skip: Icarus 13 does not support concurrent assertions
module top;
  bit clk, a, b, c;
  sequence s; @(posedge clk) (a, $display("m %0t", $time)); endsequence
  sequence s2; @(posedge clk) a ##1 b; endsequence
  property p; @(negedge clk) a; endproperty
  cover property (s);
  assert property (s2 |-> c) else $display("s2 fail %0t", $time);
  assert property (not p) else $display("not p fail %0t", $time);
  always #5 clk = !clk;
  initial begin
    a = 1; b = 1; c = 1;
    #12 a = 0;
    #10 a = 1; b = 0;
    #10 b = 1; c = 0;
    #10 $finish;
  end
endmodule
//> m 5
//> not p fail 10
//> m 25
//> not p fail 30
//> m 35
//> s2 fail 35
//> not p fail 40
