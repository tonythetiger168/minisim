// IEEE 1800-2023 7.12.3: and(), or(), xor() reductions, the result type
// being the element type.
//! icarus-skip: Icarus 13 has no and/or/xor array methods
module top;
  bit [3:0] n[$] = '{4'h3, 4'h5, 4'h6};
  byte b[] = '{8'd100, 8'd100, 8'd100};
  initial begin
    $display("%h %h %h", n.and(), n.or(), n.xor());
    $display("%0d", b.sum());
  end
endmodule

//> 0 7 0
//> 44
