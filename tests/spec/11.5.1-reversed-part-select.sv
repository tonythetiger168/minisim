// IEEE 1800-2023 11.5.1: a non-indexed part select names its more
// significant bit first. msim reports a reversed constant one as a warning
// (-Werror=range-select-reversed makes it an error) and reads it as x, so a
// design that has one in a branch a parameter turns off still runs: the
// && and || around it give their usual results (11.4.7).
//! icarus-skip: Icarus rejects the reversed select
module top;
  localparam bit WIDE = 0;
  localparam int N = 16, M = 18;
  logic [N-1:0] v = '1;
  logic en = 1;
  wire  a1 = en && WIDE && (|v[N-1:M] != 1'b0);
  wire  a2 = en || (|v[N-1:M]);
  logic p1, p2, p3;
  always_comb p1 = en && WIDE && (v[N-1:M] != 0);
  initial begin
    #1;
    p2 = |v[N-1:M];
    p3 = WIDE ? v[N-1:M] != 0 : 1'b1;
    $display("%b %b %b %b %b", a1, a2, p1, p2, p3);
  end
endmodule
//> 0 1 0 x 1
