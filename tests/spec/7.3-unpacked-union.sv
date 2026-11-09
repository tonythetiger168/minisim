// IEEE 1800-2023 7.3: An unpacked union shares storage between its members
// (7.3).
//! xfail: unpacked unions are an error
//! icarus-skip: Icarus 13 does not support unpacked unions
module top;
  typedef union packed { logic [7:0] b; logic [3:0][1:0] q; } p_t;
  typedef union { int i; p_t p; } u_t;
  u_t u;
  initial begin
    u.i = 32'h0000_00a5;
    $display("%h", u.i[7:0]);
  end
endmodule

//> a5
