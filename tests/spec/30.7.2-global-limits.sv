`timescale 1ns/1ns
// IEEE 1800-2023 30.7.2, 30.7.4: invocation options. +pulse_r/40 and
// +pulse_e/80 make the limits of a 10 delay 4 and 8: a 3-wide pulse is
// rejected, 5 becomes x, 8 passes. PATHPULSE$ takes precedence over them.
// +pulse_on_detect overrides a pulsestyle_onevent declaration, so the x
// starts when the pulse is seen.
//! args: +pulse_r/40 +pulse_e/80 +pulse_on_detect
//! icarus-skip: Icarus 13 does not filter path pulses
module d10(input a, output y);
  assign y = a;
  specify
    pulsestyle_onevent y;
    (a => y) = 10;
  endspecify
endmodule
module own(input a, output y);
  assign y = a;
  specify
    (a => y) = 10;
    specparam PATHPULSE$ = (1, 2);
  endspecify
endmodule
module top;
  reg a = 0;
  wire y, z;
  d10 u1(a, y);
  own u2(a, z);
  initial begin
    $monitor("%0t: y=%b z=%b", $time, y, z);
    #20 a = 1; #3 a = 0;
    #20 a = 1; #5 a = 0;
    #20 a = 1; #8 a = 0;
    #30 $finish;
  end
endmodule
//> 0: y=x z=x
//> 10: y=0 z=0
//> 30: y=0 z=1
//> 33: y=0 z=0
//> 48: y=x z=0
//> 53: y=x z=1
//> 58: y=0 z=0
//> 78: y=1 z=1
//> 86: y=0 z=0
