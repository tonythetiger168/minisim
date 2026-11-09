// IEEE 1800-2023 17.5: in a checker, always_ff reads sampled values (the
// value before the clock edge) while always_comb and continuous assigns
// read current values. A checker variable takes its declaration
// initializer, and a final procedure runs once per instance.
//! icarus-skip: Icarus 13 has no checkers
checker check(logic a, b, clk);
  logic x;
  logic z = 0;
  logic v;
  assign x = a;
  always_ff @(posedge clk) z <= b;
  always_comb v = !b;
  always_ff @(posedge clk) $display("%0t ff: b=%b z=%b", $time, b, z);
  final $display("final: x=%b z=%b v=%b", x, z, v);
endchecker : check

module top;
  logic a = 0, b = 0, clk = 0;
  check c1(a, b, clk);
  check c2(.a(b), .b(a), .clk(clk));
  initial begin
    #5 clk = 1; b = 1; a = 1;
    #5 clk = 0;
    #5 clk = 1;
    #1 $finish;
  end
endmodule
//> 5 ff: b=0 z=0
//> 5 ff: b=0 z=0
//> 15 ff: b=1 z=0
//> 15 ff: b=1 z=0
//> final: x=1 z=1 v=0
//> final: x=1 z=1 v=0
