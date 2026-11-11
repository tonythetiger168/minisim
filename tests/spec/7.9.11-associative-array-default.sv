// IEEE 1800-2023 7.9.11: '{default: v} gives an associative array a default
// value: an element that does not exist reads as v and is not created.
//! icarus-skip: Icarus 13 does not support associative arrays
module top;
  int d[string] = '{default: 7};
  int e[int] = '{3: 4, default: 9};
  logic [7:0] f[int];
  initial begin
    $display("%0d %0d %0d", d["x"], d.num(), d.exists("x"));
    $display("%0d %0d %0d", e[3], e[5], e.num());
    d["x"] = 1;
    $display("%0d %0d", d["x"], d["y"]);
    f = '{1: 8'h10, default: 8'hff};
    $display("%h %h", f[1], f[2]);
    f = '{2: 8'h20};
    $display("%h", f[2]);
  end
endmodule

//> 7 0 0
//> 4 9 1
//> 1 7
//> 10 ff
//> 20
