`timescale 1ns/100ps
// IEEE 1800-2023 32.4.4, Table 32-3: PORT, NETDELAY and INTERCONNECT
// annotate an interconnect delay to a load port (an input of an
// instance). A reference to the load reads the delayed value, a reference
// to the net above it the undelayed one. An INTERCONNECT whose source port
// is not found warns and annotates the load anyway. A load deeper in the
// hierarchy delays only the ports at or below it.
//! icarus-skip: Icarus 13 does not annotate interconnect delays
module drv(input i, output y);
  assign y = i;
endmodule
module bufm(input a, output y);
  assign y = a;
endmodule
module wrap(input a, output y);
  bufm b(a, y);
endmodule
module top;
  reg i = 0;
  wire n, y2, y3, y4, y5, y6;
  drv u1(i, n);
  bufm u2(n, y2);   // (INTERCONNECT u1/y u2/a (2))
  bufm u3(n, y3);   // (PORT u3/a (3) (3.5)): rise 3, fall 3.5
  bufm u4(n, y4);   // (NETDELAY u4/a (4))
  bufm u5(n, y5);   // (INTERCONNECT u9/y u5/a (5)): no u9, still annotated
  wrap u6(n, y6);   // (INTERCONNECT u1/y u6/b/a (6)): u6.a itself is not delayed
  initial begin
    $sdf_annotate("data/sdf/32.4.4-interconnect.sdf", top);
    #10 i = 1;
    #1 $display("%.1f n=%b u2.a=%b u6.a=%b u6.b.a=%b", $realtime, n, u2.a, u6.a, u6.b.a);
    #9 i = 0;
    #20 $finish;
  end
  always @(y2) if ($time > 0) $display("%.1f y2=%b", $realtime, y2);
  always @(y3) if ($time > 0) $display("%.1f y3=%b", $realtime, y3);
  always @(y4) if ($time > 0) $display("%.1f y4=%b", $realtime, y4);
  always @(y5) if ($time > 0) $display("%.1f y5=%b", $realtime, y5);
  always @(y6) if ($time > 0) $display("%.1f y6=%b", $realtime, y6);
endmodule

// At 0 the loads go from x to 0, which for u3 is the fall delay (Table 32-4).
//> 2.0 y2=0
//> 3.5 y3=0
//> 4.0 y4=0
//> 5.0 y5=0
//> 6.0 y6=0
//> 11.0 n=1 u2.a=0 u6.a=1 u6.b.a=0
//> 12.0 y2=1
//> 13.0 y3=1
//> 14.0 y4=1
//> 15.0 y5=1
//> 16.0 y6=1
//> 22.0 y2=0
//> 23.5 y3=0
//> 24.0 y4=0
//> 25.0 y5=0
//> 26.0 y6=0
