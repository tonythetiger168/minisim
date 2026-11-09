// IEEE 1800-2023 24.3.1: a nonblocking assignment in a program updates in
// the re-NBA region, and a design process it wakes runs in the next pass of
// the active region set. #0 in a program goes to the re-inactive region:
// the program finishes its reactive work before the design reacts.
//! icarus-skip: Icarus 13 runs a program in the active region, like a module
module top;
  logic [3:0] a = 0;
  logic [3:0] b = 0;
  always @(a) $display("%0t design sees a=%0d", $time, a);
  always @(b) $display("%0t design sees b=%0d", $time, b);
  tb t();
endmodule

program tb;
  initial begin
    #1;
    top.a = 1;
    #0 $display("%0t program after #0", $time);
    top.b <= 2;
    $display("%0t program b=%0d before re-NBA", $time, top.b);
    #1 $display("%0t program b=%0d", $time, top.b);
  end
endprogram

//> 1 program after #0
//> 1 program b=0 before re-NBA
//> 1 design sees a=1
//> 1 design sees b=2
//> 2 program b=2
