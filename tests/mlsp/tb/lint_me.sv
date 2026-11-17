// mlint findings for the on-save check: a latch and an unused signal.
module lint_me (input logic en, input logic [3:0] d, output logic [3:0] q);
  logic [3:0] unused_sig;
  always_comb if (en) q = d;
endmodule
