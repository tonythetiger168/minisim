// Registers that cannot leave their reset value, and the same design with the constants in.
module stuck(input logic clk, rst_n, en, input logic [3:0] d, output logic [3:0] q, output logic flag);
  logic [3:0] r;
`ifdef FOLDED
  always_ff @(posedge clk) if (en) r <= d;
  assign q = r;
`ifdef BUG
  assign flag = r[0] | r[1];
`else
  assign flag = r[0];
`endif
`else
  logic [3:0] zero;   // reset to 0, only ever cleared
  logic       one;    // reset to 1, only ever keeps its value
  logic       live;   // reset to 0 but loadable: not a constant
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin
      zero <= '0;
      one  <= 1'b1;
    end else begin
      zero <= zero & {4{en}};
      one  <= one | zero[2];
    end
  always_ff @(posedge clk) if (en) r <= d;
  assign q = r | zero;
  assign flag = one & r[0];
`endif
endmodule
