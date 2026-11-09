`timescale 1ns/1ns
// IEEE 1800-2023 30.6: with module path delays and distributed delays in
// one module, the larger of the two applies to each path. Figure 30-3: the
// path D => Q is 22 and the gates on it add up to 0 + 1, so Q follows D
// after 22. Figure 30-4: the gates add up to 10 + 20 = 30, more than the
// path's 22, so Q follows D after 30. A net delay on the output is a
// distributed delay too.
//! icarus-skip: Icarus 13 adds path and gate delays
module fig3(input A, B, C, D, output Q);
  wire n1, n2;
  and #0 g1(n1, A, B);
  and #0 g2(n2, C, D);
  or #1 g3(Q, n1, n2);
  specify
    (D => Q) = 22;
  endspecify
endmodule
module fig4(input A, B, C, D, output Q);
  wire n1, n2;
  and #10 g1(n1, A, B);
  and #10 g2(n2, C, D);
  or #20 g3(Q, n1, n2);
  specify
    (D => Q) = 22;
  endspecify
endmodule
module netd(a, y);
  input a;
  output y;
  wire #6 y;
  assign y = a;
  specify
    (a => y) = 2;
  endspecify
endmodule
module top;
  reg A = 0, B = 0, C = 1, D = 0;
  wire q3, q4, y;
  fig3 u3(A, B, C, D, q3);
  fig4 u4(A, B, C, D, q4);
  netd u5(D, y);
  initial begin
    $monitor("%0t: fig3 Q=%b fig4 Q=%b net y=%b", $time, q3, q4, y);
    #100 D = 1;
    #100 D = 0;
    #100 $finish;
  end
endmodule
//> 0: fig3 Q=x fig4 Q=x net y=x
//> 6: fig3 Q=x fig4 Q=x net y=0
//> 22: fig3 Q=0 fig4 Q=x net y=0
//> 30: fig3 Q=0 fig4 Q=0 net y=0
//> 106: fig3 Q=0 fig4 Q=0 net y=1
//> 122: fig3 Q=1 fig4 Q=0 net y=1
//> 130: fig3 Q=1 fig4 Q=1 net y=1
//> 206: fig3 Q=1 fig4 Q=1 net y=0
//> 222: fig3 Q=0 fig4 Q=1 net y=0
//> 230: fig3 Q=0 fig4 Q=0 net y=0
