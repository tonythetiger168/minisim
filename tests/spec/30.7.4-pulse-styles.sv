`timescale 1ns/1ns
// IEEE 1800-2023 30.7.4: pulse styles. As in Figure 30-6, a buffer with
// rise 4 and fall 6 gets a 2-wide 1-pulse at 10; with a reject limit of 0
// and an error limit of 6 the 4-wide output pulse is x, from its leading
// edge (on-event, the default) or from the moment the pulse is seen
// (pulsestyle_ondetect). Figure 30-7: with rise 4 and fall 6 a 1-wide
// 0-pulse schedules its trailing edge before its leading one; by default
// the leading edge is cancelled, with showcancelled the output is x between
// the two schedules (on-event) or from detection (on-detect). Figure 30-8:
// the same for a NAND whose inputs change 2 apart with delays 10 and 14.
//! icarus-skip: Icarus 13 does not filter path pulses
module b0(input in, output out);
  assign out = in;
  specify
    (in => out) = (4, 6);
    specparam PATHPULSE$ = (0, 6);
  endspecify
endmodule
module b0d(input in, output out);
  assign out = in;
  specify
    pulsestyle_ondetect out;
    (in => out) = (4, 6);
    specparam PATHPULSE$ = (0, 6);
  endspecify
endmodule
module bn(input in, output out);
  assign out = in;
  specify
    (in => out) = (4, 6);
  endspecify
endmodule
module bc(input in, output out);
  assign out = in;
  specify
    showcancelled out;
    (in => out) = (4, 6);
  endspecify
endmodule
module bcd(input in, output out);
  assign out = in;
  specify
    showcancelled out;
    pulsestyle_ondetect out;
    (in => out) = (4, 6);
  endspecify
endmodule
module nand2(input A, B, output Q);
  assign Q = !(A & B);
  specify
    (A => Q) = 10;
    (B => Q) = 14;
  endspecify
endmodule
module nand2c(input A, B, output Q);
  assign Q = !(A & B);
  specify
    showcancelled Q;
    (A => Q) = 10;
    (B => Q) = 14;
  endspecify
endmodule
module nand2cd(input A, B, output Q);
  assign Q = !(A & B);
  specify
    showcancelled Q;
    pulsestyle_ondetect Q;
    (A => Q) = 10;
    (B => Q) = 14;
  endspecify
endmodule
module top;
  reg i = 0, j = 1, A = 1, B = 0;
  wire e, d, n, c, cd, q, qc, qcd;
  b0 u1(i, e);
  b0d u2(i, d);
  bn u8(j, n);
  bc u3(j, c);
  bcd u4(j, cd);
  nand2 u5(A, B, q);
  nand2c u6(A, B, qc);
  nand2cd u7(A, B, qcd);
  initial begin
    $monitor("%0t: event=%b detect=%b | cancelled: default=%b event=%b detect=%b | nand: default=%b event=%b detect=%b",
             $time, e, d, n, c, cd, q, qc, qcd);
    #10 i = 1; j = 0;
    #1 j = 1;
    #1 i = 0;
    #18 B = 1;
    #2 A = 0;
    #30 $finish;
  end
endmodule
//> 0: event=x detect=x | cancelled: default=x event=x detect=x | nand: default=x event=x detect=x
//> 4: event=x detect=x | cancelled: default=1 event=1 detect=1 | nand: default=x event=x detect=x
//> 6: event=0 detect=0 | cancelled: default=1 event=1 detect=1 | nand: default=x event=x detect=x
//> 10: event=0 detect=0 | cancelled: default=1 event=1 detect=1 | nand: default=1 event=1 detect=1
//> 11: event=0 detect=0 | cancelled: default=1 event=1 detect=x | nand: default=1 event=1 detect=1
//> 12: event=0 detect=x | cancelled: default=1 event=1 detect=x | nand: default=1 event=1 detect=1
//> 14: event=x detect=x | cancelled: default=1 event=1 detect=x | nand: default=1 event=1 detect=1
//> 15: event=x detect=x | cancelled: default=1 event=x detect=x | nand: default=1 event=1 detect=1
//> 16: event=x detect=x | cancelled: default=1 event=1 detect=1 | nand: default=1 event=1 detect=1
//> 18: event=0 detect=0 | cancelled: default=1 event=1 detect=1 | nand: default=1 event=1 detect=1
//> 32: event=0 detect=0 | cancelled: default=1 event=1 detect=1 | nand: default=1 event=1 detect=x
//> 42: event=0 detect=0 | cancelled: default=1 event=1 detect=1 | nand: default=1 event=x detect=x
//> 44: event=0 detect=0 | cancelled: default=1 event=1 detect=1 | nand: default=1 event=1 detect=1
