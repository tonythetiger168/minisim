// IEEE 1800-2023 28.16.2 trireg charge decay and Annex E optional compiler
// directives. The third delay of a trireg is its charge decay time: when
// its drivers turn off it holds its 0 or 1 for that long, then becomes x,
// unless a driver turns on first; a decay time of 0 makes it x at once.
// `default_decay_time (E.2) is the decay time of a trireg that gives none,
// `default_trireg_strength (E.3) its charge strength when it names none
// (4: large). `delay_mode_zero (E.7) drops gate, net and continuous
// assignment delays, `delay_mode_unit (E.6) makes each nonzero delay one
// unit of precision, `delay_mode_distributed (E.4) ignores module path
// delays and `delay_mode_path (E.5) keeps only them. `resetall clears all.
//! icarus-skip: Icarus does not model charge decay or the delay modes
`timescale 1ns / 1ns
`default_decay_time 30
`default_trireg_strength 4
module dflt;
  reg en = 1, d = 1;
  trireg t;          // decays after 30 (`default_decay_time), large charge
  trireg (small) #(0, 0, 50) s;   // its own decay time 50
  trireg #(0, 0, 0) z;            // decays at once
  assign t = en ? d : 1'bz;
  assign s = en ? d : 1'bz;
  assign z = en ? d : 1'bz;
  initial begin
    #10 en = 0;
    #1 $display("%0t t=%b %v s=%b %v z=%b", $time, t, t, s, s, z);
    #28 $display("%0t t=%b s=%b", $time, t, s);
    #2 $display("%0t t=%b s=%b", $time, t, s);
    en = 1; d = 0;
    #1 en = 0;
    #20 en = 1;   // driven again before the decay: no x
    #1 en = 0;
    #28 $display("%0t t=%b", $time, t);
    #3 $display("%0t t=%b", $time, t);
    #20 $display("%0t s=%b", $time, s);
  end
endmodule
`resetall
`timescale 1ns / 1ns
`delay_mode_zero
module mzero(output y, input a);
  assign #5 y = a;
endmodule
`delay_mode_unit
module munit(output y, input a);
  assign #5 y = a;
  not #(3, 7) g(y2, a);
endmodule
`delay_mode_distributed
module mdist(output y, input a);
  assign #2 y = a;
  specify (a => y) = 6; endspecify
endmodule
`delay_mode_path
module mpath(output y, input a);
  assign #2 y = a;
  specify (a => y) = 6; endspecify
endmodule
`resetall
`timescale 1ns / 1ns
module top;
  reg a = 0;
  wire y0, y1, y2, y3;
  dflt u0();
  mzero z(y0, a);
  munit u(y1, a);
  mdist d(y2, a);
  mpath p(y3, a);
  initial begin
    #100 a = 1;
    #0.5;
  end
  always @(y0) if ($time > 0) $display("%0t zero y=%b", $time, y0);
  always @(y1) if ($time > 0) $display("%0t unit y=%b", $time, y1);
  always @(y2) if ($time > 0) $display("%0t dist y=%b", $time, y2);
  always @(y3) if ($time > 0) $display("%0t path y=%b", $time, y3);
endmodule
//> 1 unit y=0
//> 2 dist y=0
//> 6 path y=0
//> 11 t=1 La1 s=1 Sm1 z=x
//> 39 t=1 s=1
//> 41 t=x s=1
//> 91 t=0
//> 94 t=x
//> 100 zero y=1
//> 101 unit y=1
//> 102 dist y=1
//> 106 path y=1
//> 114 s=x
