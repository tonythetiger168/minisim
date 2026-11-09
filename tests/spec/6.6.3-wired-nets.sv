// IEEE 1800-2023 6.6.3: wand/triand resolve with AND, wor/trior with OR
// (6.6.3); tri0/tri1 pull an undriven net (6.6.5); supply0/supply1 (6.6.6).
//! xfail: wand/wor resolve like wire
module top;
  logic a = 1, b = 0;
  wand wa; triand ta; wor wo; trior to;
  assign wa = a; assign wa = b;
  assign ta = a; assign ta = 1'b1;
  assign wo = a; assign wo = b;
  assign to = b; assign to = 1'b0;
  tri0 t0; tri1 t1;
  supply0 s0; supply1 s1;
  wand wz; assign wz = 1'bz; assign wz = 1'b1;
  initial #1 $display("%b %b %b %b %b %b %b %b %b", wa, ta, wo, to, t0, t1, s0, s1, wz);
endmodule

//> 0 1 1 0 0 1 0 1 1
