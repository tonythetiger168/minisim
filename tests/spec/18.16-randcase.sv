// IEEE 1800-2023 18.16: randcase picks a branch by weight; a zero weight is
// never chosen (18.16).
//! xfail: randcase is an error
//! icarus-skip: Icarus 13 does not support randcase
module top;
  int n[3] = '{0, 0, 0};
  initial begin
    repeat (200) randcase
      1: n[0]++;
      3: n[1]++;
      0: n[2]++;
    endcase
    $display("%0d %0d %0d", n[0] > 0, n[1] > n[0], n[2]);
  end
endmodule

//> 1 1 0
