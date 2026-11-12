// IEEE 1800-2023 17.7.2: in simulation the active free variables of a
// checker with assume statements are solved at each tick of the assume
// set's clock, before the observed region: they take random values that
// keep every assumption from failing in that time step, and hold them until
// the next tick. The run is the same for a given seed.
//! icarus-skip: Icarus 13 has no checkers
//> Error: "17.7.2-assume-free-variables.sv", 31: top.u.m: at time 3
//> Error: "17.7.2-assume-free-variables.sv", 31: top.u.m: at time 13
//> held between clocks 1, flag seen 1
checker pick(logic clk, logic rst);
  default clocking @(posedge clk); endclocking
  rand bit [3:0] a, b;
  rand bit flag;
  rand bit [7:0] free;
  m1: assume property (disable iff (rst) a + b == 10);
  m2: assume property (flag |-> a > b);
  m3: assume property (a != 0 && !(b == 5));
  m4: assume property (flag |=> !flag);   // next tick: solved with the previous flag
  bit kept = 1, seen = 0;
  bit [3:0] q;
  always_ff @(posedge clk) q <= a;
  always_ff @(negedge clk) if ($time > 5 && a != q) kept <= 0;
  a1: assert property (rst || (a + b == 10 && a != 0 && b != 5 && (!flag || a > b)));
  a2: assert property (flag |=> !flag);
  always_ff @(posedge clk) if (flag) seen <= 1;
  final $display("held between clocks %0b, flag seen %0b", kept, seen);
endchecker
checker never(logic clk);
  default clocking @(posedge clk); endclocking
  rand bit [3:0] v;
  m: assume property (v > 20);   // no solution: the attempt fails, and so does m
endchecker
module top;
  logic clk = 0, rst = 0;
  pick p(clk, rst);
  logic slow = 0;
  never u(slow);
  initial begin #3 slow = 1; #5 slow = 0; #5 slow = 1; end
  initial begin repeat (60) #5 clk = ~clk; $finish; end
endmodule
