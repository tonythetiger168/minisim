// IEEE 1800-2023 21.7.4.3: in an extended VCD file, an inout port's state
// says which side drives it. Drivers inside the module are the DUT side,
// the rest the test fixture side. Both sides with the same value give 0/1
// at equal strength range, d/u when only the fixture drives strongly, l/h
// when only the DUT does; different values give A/a/B/b/C/c. Two active
// drivers on one side give d/u (input) or l/h (output).
//! icarus-skip: Icarus 13 ignores $dumpports
module pad(inout io, input [1:0] mode, input en2, output o2);
  assign (strong0, strong1) io = mode == 0 ? 1'bz : mode == 1 ? 1'b0 : mode == 2 ? 1'b1 : 1'bx;
  assign o2 = en2 ? 1'b1 : 1'bz;
  assign o2 = en2 ? 1'b1 : 1'bz;
endmodule
module top;
  logic [1:0] mode = 0, tb = 0;
  logic weak_tb = 0, en2 = 0;
  wire io, o2;
  assign io = tb == 0 ? 1'bz : tb == 1 ? 1'b0 : tb == 2 ? 1'b1 : 1'bx;
  assign (weak0, weak1) io = weak_tb ? 1'b0 : 1'bz;
  pad u(io, mode, en2, o2);
  int fd;
  string line;
  initial begin
    $dumpports(u, "spec_21_7_4_3.evcd");
    #1 tb = 1;                // fixture only: D
    #1 mode = 1;              // both 0, both strong: 0
    #1 tb = 0; weak_tb = 1;   // DUT strong, fixture weak: l
    #1 weak_tb = 0; tb = 1; mode = 2;   // fixture 0, DUT 1: A
    #1 tb = 2; mode = 1;      // B
    #1 tb = 3;                // C
    #1 tb = 0; mode = 0; en2 = 1;   // nobody on io: F; two drivers on o2: h
    #1 $dumpportsflush;
    fd = $fopen("spec_21_7_4_3.evcd", "r");
    while ($fgets(line, fd)) if (line[0] == "p" || line[0] == "#") $write("%s", line);
    $fclose(fd);
  end
endmodule
//> #0
//> pf 0 0 <0
//> pDD 66 00 <1
//> pD 6 0 <2
//> pT 0 0 <3
//> #1
//> pD 6 0 <0
//> #2
//> p0 6 0 <0
//> pDU 60 06 <1
//> #3
//> pl 6 0 <0
//> #4
//> pA 6 6 <0
//> pUD 06 60 <1
//> #5
//> pB 6 6 <0
//> pDU 60 06 <1
//> #6
//> pC 6 6 <0
//> #7
//> pf 0 0 <0
//> pDD 66 00 <1
//> pU 0 6 <2
//> ph 0 6 <3
