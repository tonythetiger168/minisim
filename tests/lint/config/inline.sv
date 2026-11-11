// Waivers in the source. All seven lines below truncate.
module inl(input logic [7:0] a, output logic [3:0] y0, y1, y2, y3, y4, y5,
           output logic f);
  // mlint: waive W001 intended truncation
  assign y0 = a;
  // the comment on the line itself:
  assign y1 = a;  // mlint: waive width
  // mlint: off W001
  assign y2 = a;
  assign y3 = a;
  // mlint: on W001
  // a waiver for another rule does not hide this one:
  assign y4 = a;  // mlint: waive W004
  // several rules at once:
  assign {f, y5} = {a == y0, a};  // mlint: waive W001,W004
endmodule
