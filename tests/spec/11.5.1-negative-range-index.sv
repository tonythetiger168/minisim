// 11.5.1: a run-time index into a vector whose range has negative indices.
// An index expression such as {1'b0, n} - 16 is 32-bit unsigned; msim reads
// it as an integer, as other simulators do, so v[n-16 +: 4] names bits of a
// [15:-16] vector (as unsigned it could never be inside that range).
//! icarus-skip: Icarus does not write the part select
module top;
  logic [15:-16] v = 32'h1234_5678;
  logic [3:0] n;
  logic [5:0] m;
  logic [3:0] r;
  initial begin
    n = 4'd4;
    r = v[{1'b0, n} - 16 +: 4];        // bits -12..-9
    $display("%h", r);
    r = v[{1'b0, n} - 10 -: 4];        // bits -9..-6
    $display("%h", r);
    m = 6'd0;
    r = v[m - 6 +: 4];                 // bits -6..-3
    $display("%h", r);
    v[{1'b0, n} - 16 +: 4] = 4'hc;
    $display("%h", v);
  end
endmodule
//> 7
//> c
//> 5
//> 123456c8
