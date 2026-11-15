// IEEE 1800-2023 10.3 / 9.4.2: a continuous assignment of a constant gives
// its net the value at time 0; procedures that wait on the net see it.
module t;
  wire [7:0] k;
  reg [7:0] a, b;
  assign k = 8'd5;
  always @* a = k + 1;
  always @(k) b = k;
  initial #1 $display("%0d %0d", a, b);
endmodule
//> 6 5
