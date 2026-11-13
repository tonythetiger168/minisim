// IEEE 1800-2023 16.9.4: a global clocking future sampled value function
// shall not be used in an assertion that contains sequence match items.
// msim reports it as an error.
//! icarus-skip: Icarus has no global clocking future functions
//! exit: 1
module t;
  bit clk, a, b;
  global clocking gc @(posedge clk); endclocking
  always #5 clk = !clk;
  sequence s;
    bit v;
    (a, v = b) ##1 (b == v);
  endsequence
  a_bad: assert property (@($global_clock) s |-> $rising_gclk(a));
endmodule
