// IEEE 1800-2023 11.4.13: inside with an unpacked array or queue as an item
// (any element matches, with ==?), and real values and ranges.
//! icarus-skip: Icarus 13 does not support these inside items
module top;
  int arr[4] = '{1, 2, 3, 4};
  int q[$] = '{10, 20};
  logic [3:0] pat[2] = '{4'b1?0?, 4'b0000};
  real r = 2.5;
  initial begin
    $display("%0d %0d", 3 inside {arr}, 7 inside {arr});
    $display("%0d %0d", 20 inside {q, 99}, 21 inside {q});
    $display("%0d %0d", 4'b1101 inside {pat}, 4'b0110 inside {pat});
    $display("%0d %0d", r inside {[2.0:3.0]}, r inside {1.5, 2.5});
    q.push_back(7);
    $display("%0d", 7 inside {q});
  end
endmodule
//> 1 0
//> 1 0
//> 1 0
//> 1 1
//> 1
