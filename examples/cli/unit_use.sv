module unit_top;
  logic [`UNIT_W-1:0] x;
  unit_cell c ();
  initial begin
    x = '1;
    $display("w=%0d x=%0d twice=%0d", $bits(x), x, unit_twice(21));
  end
endmodule
