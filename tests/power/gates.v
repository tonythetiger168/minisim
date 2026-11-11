// A gate-level netlist for the mpower tests: three cells, hand-checkable.
module dut(input clk, input a, input b, output y);
  wire n1, n2;
  INV   u1(.A(a), .Y(n1));
  NAND2 u2(.A(n1), .B(b), .Y(n2));
  DFF   r(.D(n2), .CK(clk), .Q(y));
endmodule
