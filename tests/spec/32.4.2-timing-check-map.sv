`timescale 1ns/1ns
// IEEE 1800-2023 32.4.2, Table 32-2: which SystemVerilog timing checks
// each SDF timing check annotates, and which of their limits. Every check
// starts with a limit that the stimulus meets; only the annotated limit is
// violated, so each report shows an annotation took effect.
//! icarus-skip: Icarus 13 does not run timing checks
module checks(input c1, d1, c2, d2, c3, d3, r4, c4, r5, c5, r6, c6, c7, d7,
              w8, p9, n10, d10, a11, b11, c12, d12);
  specify
    $setup(d1, posedge c1, 1);              // SETUP 5
    $hold(posedge c2, d2, 1);               // HOLD 5
    $setuphold(posedge c3, d3, 1, 1);       // SETUP 5, then HOLD 6
    $recovery(posedge r4, posedge c4, 1);   // RECOVERY 5
    $removal(posedge r5, posedge c5, 1);    // REMOVAL 5
    $recrem(posedge r6, posedge c6, 1, 1);  // RECREM 5 6
    $skew(posedge c7, posedge d7, 5);       // SKEW 2
    $width(posedge w8, 1);                  // WIDTH 5
    $period(posedge p9, 1);                 // PERIOD 5
    $nochange(posedge n10, d10, 0, 0);      // NOCHANGE 0 2
    $fullskew(posedge a11, posedge b11, 5, 5);  // BIDIRECTSKEW 2 2
    $timeskew(posedge c12, posedge d12, 5); // SKEW 2
  endspecify
endmodule
module top;
  reg c1 = 0, d1 = 0, c2 = 0, d2 = 0, c3 = 0, d3 = 0, r4 = 0, c4 = 0, r5 = 0, c5 = 0, r6 = 0, c6 = 0,
      c7 = 0, d7 = 0, w8 = 0, p9 = 0, n10 = 0, d10 = 0, a11 = 0, b11 = 0, c12 = 0, d12 = 0;
  checks u(c1, d1, c2, d2, c3, d3, r4, c4, r5, c5, r6, c6, c7, d7, w8, p9, n10, d10, a11, b11, c12, d12);
  initial begin
    $sdf_annotate("data/sdf/32.4.2-timing-check-map.sdf", top);
    #20 d1 = 1; d3 = 1; r4 = 1; c5 = 1; r6 = 1; w8 = 1; p9 = 1; n10 = 1; a11 = 1; c12 = 1;
    #3  c1 = 1; c3 = 1; c4 = 1; r5 = 1; c6 = 1; w8 = 0; p9 = 0; n10 = 0; c2 = 1; c7 = 1; b11 = 1; d12 = 1;
    #1  d3 = 0; d10 = 1; p9 = 1;
    #3  d2 = 1; d7 = 1;
    #20 $finish;
  end
endmodule

//> "32.4.2-timing-check-map.sv", 20: Timing violation in top.u
//>     $fullskew( posedge a11:20, posedge b11:22, 2, 2 );
//> "32.4.2-timing-check-map.sv", 21: Timing violation in top.u
//>     $timeskew( posedge c12:20, posedge d12:22, 2 );
//> "32.4.2-timing-check-map.sv", 10: Timing violation in top.u
//>     $setup( d1:20, posedge c1:23, 5 );
//> "32.4.2-timing-check-map.sv", 12: Timing violation in top.u
//>     $setuphold( posedge c3:23, d3:20, 5, 6 );
//> "32.4.2-timing-check-map.sv", 13: Timing violation in top.u
//>     $recovery( posedge r4:20, posedge c4:23, 5 );
//> "32.4.2-timing-check-map.sv", 14: Timing violation in top.u
//>     $removal( posedge r5:23, posedge c5:20, 5 );
//> "32.4.2-timing-check-map.sv", 15: Timing violation in top.u
//>     $recrem( posedge r6:20, posedge c6:23, 5, 6 );
//> "32.4.2-timing-check-map.sv", 17: Timing violation in top.u
//>     $width( posedge w8:20, :23, 5 );
//> "32.4.2-timing-check-map.sv", 12: Timing violation in top.u
//>     $setuphold( posedge c3:23, d3:24, 5, 6 );
//> "32.4.2-timing-check-map.sv", 18: Timing violation in top.u
//>     $period( posedge p9:20, :24, 5 );
//> "32.4.2-timing-check-map.sv", 19: Timing violation in top.u
//>     $nochange( posedge n10:20, d10:24, 0, 2 );
//> "32.4.2-timing-check-map.sv", 20: Timing violation in top.u
//>     $fullskew( posedge a11:25, posedge b11:23, 2, 2 );
//> "32.4.2-timing-check-map.sv", 11: Timing violation in top.u
//>     $hold( posedge c2:23, d2:27, 5 );
//> "32.4.2-timing-check-map.sv", 16: Timing violation in top.u
//>     $skew( posedge c7:23, posedge d7:27, 2 );
