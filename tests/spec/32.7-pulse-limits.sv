`timescale 1ns/1ns
// IEEE 1800-2023 32.7, 32.5: SDF annotation of pulse limits. With
// +pulse_r/40 +pulse_e/80, (IOPATH a y (5)) gives a delay of 5, a reject
// limit of 2 and an error limit of 4 (u1); ((5) () ()) keeps the limits,
// 0 for a path that had delay 0 (u2); INCREMENT (() (-4) (-5)) takes limits
// of 3 down to 0 (u3). PATHPULSE (u4) and PATHPULSEPERCENT (u5) set only the
// limits; a later IOPATH overwrites them (u6), ((10) (1) (2)) sets all three
// (u7), and limits above the delay act as the delay (u8). Pulses of widths
// 1 to 5 go to every instance.
//! args: +pulse_r/40 +pulse_e/80
//! icarus-skip: Icarus 13 does not annotate SDF
module c0(input a, output y);
  assign y = a;
  specify
    (a => y) = 0;
  endspecify
endmodule
module c6(input a, output y);
  assign y = a;
  specify
    (a => y) = 6;
    specparam PATHPULSE$ = 3;
  endspecify
endmodule
module top;
  reg a = 0;
  wire [8:1] y;
  c0 u1(a, y[1]);
  c0 u2(a, y[2]);
  c6 u3(a, y[3]);
  c0 u4(a, y[4]);
  c0 u5(a, y[5]);
  c0 u6(a, y[6]);
  c0 u7(a, y[7]);
  c0 u8(a, y[8]);
  integer w;
  initial begin
    $sdf_annotate("data/sdf/32.7-pulse-limits.sdf", top);
    #20;
    for (w = 1; w <= 5; w = w + 1) begin
      a = 1; #(w) a = 0;
      #30;
      $display("width %0d: u8..u1 saw %b", w, seen);
      seen = 0;
    end
    $finish;
  end
  // What each output did: 1 = passed the pulse, x = went to x, 0 = nothing.
  reg [8:1] seen = 0;
  genvar k;
  for (k = 1; k <= 8; k++) begin : watch
    always @(y[k]) if ($time > 15) begin
      if (y[k] === 1'bx) seen[k] = 1'bx;
      else if (y[k] === 1'b1 && seen[k] !== 1'bx) seen[k] = 1'b1;
    end
  end
endmodule
//> width 1: u8..u1 saw 0x00x110
//> width 2: u8..u1 saw 010xx11x
//> width 3: u8..u1 saw 010x111x
//> width 4: u8..u1 saw 01x11111
//> width 5: u8..u1 saw 11x11111
