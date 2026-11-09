// IEEE 1800-2023 7.3.2: Tagged unions: tagged expressions and pattern
// matching with matches (7.3.2, 11.9, 12.6).
//! xfail: tagged unions are an error
//! icarus-skip: Icarus 13 does not support tagged unions
module top;
  typedef union tagged { int I; bit [3:0] N; void None; } u_t;
  u_t u;
  initial begin
    u = tagged I 5;
    if (u matches tagged I .v) $display("I %0d", v);
    u = tagged N 4'h3;
    case (u) matches
      tagged I .v: $display("I %0d", v);
      tagged N .n: $display("N %0d", n);
    endcase
  end
endmodule

//> I 5
//> N 3
