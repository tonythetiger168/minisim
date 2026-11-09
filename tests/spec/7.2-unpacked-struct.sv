// IEEE 1800-2023 7.2: An unpacked struct: member read and write, a struct
// literal, whole-struct copy and compare (7.2, 5.10).
//! xfail: unpacked structs are an error
//! icarus-skip: Icarus 13 does not compare unpacked structs
module top;
  typedef struct { int a; logic [7:0] b; string s; } s_t;
  s_t x, y;
  initial begin
    x.a = 1; x.b = 8'h2; x.s = "hi";
    $display("%0d %h %s", x.a, x.b, x.s);
    y = '{5, 8'hff, "yo"};
    $display("%0d %h %s", y.a, y.b, y.s);
    y = x;
    $display("%0d %0d", y.a, y == x);
  end
endmodule

//> 1 02 hi
//> 5 ff yo
//> 1 1
