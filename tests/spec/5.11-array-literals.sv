// IEEE 1800-2023 5.11: Array literals: positional, replicated and default:
// (5.11, 10.9.1).
//! xfail: assignment patterns to a fixed-size unpacked array
//! icarus-skip: Icarus 13 does not parse nested array patterns
module top;
  int a[3];
  int b[2][2];
  initial begin
    a = '{1, 2, 3};
    $display("%0d %0d %0d", a[0], a[1], a[2]);
    a = '{3{7}};
    $display("%0d", a[2]);
    a = '{default: 9};
    $display("%0d", a[1]);
    b = '{'{1, 2}, '{3, 4}};
    $display("%0d %0d", b[0][1], b[1][0]);
  end
endmodule

//> 1 2 3
//> 7
//> 9
//> 2 3
