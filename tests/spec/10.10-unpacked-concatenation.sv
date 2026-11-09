// IEEE 1800-2023 10.10: Unpacked array concatenation into a fixed array
// (10.10).
//! xfail: unpacked concatenation into a fixed array is an error
//! icarus-skip: Icarus 13 assigns zeros
module top;
  int a[3], b[5];
  initial begin
    a = {1, 2, 3};
    $display("%0d %0d %0d", a[0], a[1], a[2]);
    b = {a, 4, 5};
    $display("%0d %0d", b[2], b[4]);
  end
endmodule

//> 1 2 3
//> 3 5
