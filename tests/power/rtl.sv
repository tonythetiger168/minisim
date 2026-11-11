// The RTL the netlist in gates.v was made from.
module dut(input logic clk, input logic a, input logic b, output logic y);
  logic n2;
  assign n2 = ~(~a & b);
  always_ff @(posedge clk) y <= n2;
endmodule
