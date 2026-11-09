// IEEE 1800-2023 20.4: $printtimescale reports a module's unit and
// precision (20.4.2).
`timescale 1ns/1ps
module top;
  initial $printtimescale;
endmodule

//> Time scale of (top) is 1ns / 1ps
