// IEEE 1800-2023 17.6 / 17.8: a covergroup declared and instantiated in a
// checker samples on its event; a function declared in the checker is
// called from the checker's procedures.
//! icarus-skip: Icarus 13 has no checkers
checker cc(logic [1:0] v, event ck);
  covergroup g @(ck); coverpoint v; endgroup
  g inst = new;
  function automatic int twice(int x); return 2 * x; endfunction
  logic [3:0] d;
  always_ff @(ck) d <= twice(v);
  final $display("cov=%0.1f d=%0d", inst.get_coverage(), d);
endchecker
module top;
  bit clk; logic [1:0] v = 0;
  always #5 clk = ~clk;
  cc c(v, posedge clk);
  initial begin repeat (3) @(negedge clk) v++; #10 $finish; end
endmodule
//> cov=100.0 d=6
