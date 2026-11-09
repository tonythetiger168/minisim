// IEEE 1800-2023 24.3.2: a continuous assignment in a program runs in the
// reactive region; a design process sensitive to the net it drives wakes
// in the next pass of the active region set.
//! icarus-skip: Icarus 13 runs a program in the active region, like a module
module m;
  logic r;
  wire dw1, dw2;
  initial begin
    r = 0;
    #10 r = 1;
  end
  assign dw1 = r;
  p p_i(dw2, dw1);
  always @(dw2)
    $display("%0t dw2 is %b", $time, dw2);
endmodule

program p(output pw2, input pw1);
  assign pw2 = pw1;
endprogram

//> 0 dw2 is 0
//> 10 dw2 is 1
