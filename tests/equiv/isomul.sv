// Operand isolation: the multiplier's operands are forced to 0 when the
// product is not used (ISO). Equal only case by case on the select.
module isomul(input logic clk, rst, use_mul, input logic signed [11:0] sa, sb,
              output logic signed [23:0] sp);
`ifdef ISO
  wire on = !rst && use_mul;
`ifdef BUG
  always_ff @(posedge clk)
    if (rst) sp <= '0;
    else if (use_mul) sp <= (on ? sa : $signed(1'b0)) * (rst ? sb : $signed(1'b0));
`else
  always_ff @(posedge clk)
    if (rst) sp <= '0;
    else if (use_mul) sp <= (on ? sa : $signed(1'b0)) * (on ? sb : $signed(1'b0));
`endif
`else
  always_ff @(posedge clk)
    if (rst) sp <= '0;
    else if (use_mul) sp <= sa * sb;
`endif
endmodule
