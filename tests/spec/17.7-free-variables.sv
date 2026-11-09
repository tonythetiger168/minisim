// IEEE 1800-2023 17.7, 17.7.2: free checker variables. A const one keeps
// one random value; one the checker assigns (a toggling variable) starts
// random and then follows its assignments; an active one, never assigned,
// takes a new random value in each time step. With a fixed seed the run
// is the same every time.
//! icarus-skip: Icarus 13 has no checkers
checker free_vars(logic clk);
  rand const bit [5:0] idx;
  rand bit t;
  rand bit [7:0] r;
  bit varied = 0;
  always_ff @(posedge clk) if ($time > 5 && r != $past(r, 1, 1, @(posedge clk))) varied <= 1;
  final $display("r varied: %b", varied);
  always_ff @(posedge clk) t <= !t;
  always_ff @(posedge clk) $display("idx same: %b, t toggles: %b, r is known: %b",
                                    idx == $past(idx, 1, 1, @(posedge clk)) || $time == 5,
                                    t != $past(t, 1, 1, @(posedge clk)) || $time == 5,
                                    !$isunknown(r));
endchecker
module top;
  logic clk = 0;
  free_vars f(clk);
  initial begin
    repeat (12) #5 clk = ~clk;
    $finish;
  end
endmodule
//> idx same: 1, t toggles: 1, r is known: 1
//> idx same: 1, t toggles: 1, r is known: 1
//> idx same: 1, t toggles: 1, r is known: 1
//> idx same: 1, t toggles: 1, r is known: 1
//> idx same: 1, t toggles: 1, r is known: 1
//> idx same: 1, t toggles: 1, r is known: 1
//> r varied: 1
