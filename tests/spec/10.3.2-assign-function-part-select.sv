// IEEE 1800-2023 10.3.2, 9.2.2.2.1, 13.4: a function that builds its result
// (and a local) one part-select at a time in a loop, called from two
// continuous assignments and an always_comb. The function's own variables
// are not operands of the callers, so the callers do not wake each other
// forever (msim used to hang at time 0 on this shape).
module top;
  function automatic logic [31:0] spread(input logic [3:0] n);
    logic [31:0] t;
    for (int i = 0; i < 8; i++) begin
      t[4 * i +: 4] = n + 4'(i);
      spread[31 - 4 * i -: 4] = t[4 * i +: 4];
    end
  endfunction
  function logic [15:0] mask(input logic [2:0] n);   // static
    for (int i = 0; i < 8; i++) mask[2 * i +: 2] = i < n ? 2'b11 : 2'b00;
  endfunction
  logic [3:0] a = 1, b = 9;
  logic [2:0] c = 3;
  logic [31:0] x, y, z;
  logic [15:0] m1, m2;
  assign x = spread(a);
  assign y = spread(b);
  assign m1 = mask(c);
  assign m2 = mask(3'd5);
  always_comb z = spread(a ^ b);
  initial begin
    #1 $display("%h %h %h %h %h", x, y, z, m1, m2);
    a = 4'hf; c = 7;
    #1 $display("%h %h %h %h %h", x, y, z, m1, m2);
  end
endmodule
//> 12345678 9abcdef0 89abcdef 003f 03ff
//> f0123456 9abcdef0 6789abcd 3fff 03ff
