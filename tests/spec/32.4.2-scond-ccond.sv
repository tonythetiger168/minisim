`timescale 1ns/1ns
// SDF SCOND / CCOND (IEEE 1497) on SETUPHOLD name the timestamp and
// timecheck conditions of a $setuphold or $recrem (IEEE 1800-2023 31.9.2),
// and like COND (32.4.2) they must match: d1's check has both, d2's has
// !en where the SDF says en, d3's has none. An SDF check without them
// matches any check (d4). SCOND on SETUP is ignored with a warning (d5).
// The limits start at 1; each annotated setup limit of 5 is violated.
//! icarus-skip: Icarus 13 does not run timing checks
module chk(input en, c1, d1, c2, d2, c3, d3, c4, d4, c5, d5);
  specify
    $setuphold(posedge c1, d1, 1, 1, , en, en);
    $setuphold(posedge c2, d2, 1, 1, , !en, );
    $setuphold(posedge c3, d3, 1, 1);
    $setuphold(posedge c4, d4, 1, 1, , en, );
    $setup(d5, posedge c5, 1);
  endspecify
endmodule
module top;
  reg en = 1, c1 = 0, d1 = 0, c2 = 0, d2 = 0, c3 = 0, d3 = 0, c4 = 0, d4 = 0, c5 = 0, d5 = 0;
  chk u(en, c1, d1, c2, d2, c3, d3, c4, d4, c5, d5);
  initial begin
    $sdf_annotate("data/sdf/32.4.2-scond-ccond.sdf", top, , "/dev/stdout");
    #20 d1 = 1; d2 = 1; d3 = 1; d4 = 1; d5 = 1;
    #3 c1 = 1; c2 = 1; c3 = 1; c4 = 1; c5 = 1;
    #10 $finish;
  end
endmodule
//> SDF data/sdf/32.4.2-scond-ccond.sdf -> top (TYPICAL)
//> data/sdf/32.4.2-scond-ccond.sdf:6: SETUPHOLD d1 c1 -> 1 check in top.u
//> Warning: data/sdf/32.4.2-scond-ccond.sdf:7: no timing check in top.u for SETUPHOLD d2 c2
//> Warning: data/sdf/32.4.2-scond-ccond.sdf:8: no timing check in top.u for SETUPHOLD d3 c3
//> data/sdf/32.4.2-scond-ccond.sdf:9: SETUPHOLD d4 c4 -> 1 check in top.u
//> Warning: data/sdf/32.4.2-scond-ccond.sdf:10: SETUP: SCOND / CCOND belong to SETUPHOLD and RECREM; ignoring them
//> data/sdf/32.4.2-scond-ccond.sdf:10: SETUP d5 c5 -> 1 check in top.u
//> 3 annotations, 3 warnings
//> "32.4.2-scond-ccond.sv", 11: Timing violation in top.u
//>     $setuphold( posedge c1:23, d1:20, 5, 1 );
//> "32.4.2-scond-ccond.sv", 14: Timing violation in top.u
//>     $setuphold( posedge c4:23, d4:20, 5, 1 );
//> "32.4.2-scond-ccond.sv", 15: Timing violation in top.u
//>     $setup( d5:20, posedge c5:23, 5 );
