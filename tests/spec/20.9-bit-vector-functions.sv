// IEEE 1800-2023 20.9: $countbits, $countones, $onehot, $onehot0,
// $isunknown (20.9).
module top;
  initial begin
    $display("%0d %0d %0d", $countones(8'hf0), $countbits(8'b1x0z_1100, 1'b1), $countbits(8'b1x0z_1100, 1'bx, 1'bz));
    $display("%0d %0d %0d %0d", $onehot(4'b0100), $onehot(4'b0110), $onehot0(4'b0000), $onehot0(4'b0011));
    $display("%0d %0d", $isunknown(4'b1x00), $isunknown(4'b1100));
  end
endmodule

//> 4 3 2
//> 1 0 1 0
//> 1 0
