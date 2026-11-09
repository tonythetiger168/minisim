// IEEE 1800-2023 21.2.1.7: %p prints a value as an assignment pattern
// (21.2.1.7).
//! xfail: %p crashes msim
//! icarus-skip: Icarus 13 prints %p differently
module top;
  int a[3] = '{1, 2, 3};
  int q[$] = '{4, 5};
  typedef struct packed { logic [3:0] x, y; } s_t;
  s_t s = '{4'h1, 4'h2};
  initial begin
    $display("%p", a);
    $display("%p", q);
    $display("%p", s);
    $display("%p", 8'd7);
  end
endmodule

//> '{1, 2, 3}
//> '{4, 5}
//> '{x:'h1, y:'h2}
//> 7
