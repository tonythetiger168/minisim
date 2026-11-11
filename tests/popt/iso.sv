// Operand isolation: a multiplier and a divider whose results are used
// under a condition, with signed and unsigned operands.
module iso (
  input  logic               clk,
  input  logic               rst,
  input  logic               use_mul,
  input  logic [1:0]         mode,
  input  logic signed [11:0] sa,
  input  logic signed [11:0] sb,
  input  logic [11:0]        ua,
  input  logic [11:0]        ub,
  output logic signed [23:0] sp,
  output logic [23:0]        up,
  output logic [11:0]        q,
  output logic [23:0]        y
);
  always_ff @(posedge clk)
    if (rst) begin
      sp <= '0;
      up <= '0;
    end else begin
      if (use_mul) sp <= sa * sb;
      case (mode)
        2'd1: up <= ua * ub;
        2'd2: up <= {12'd0, ua} + 24'd1;
        default: ;
      endcase
    end

  assign y = use_mul ? ua * (ub + 12'd3) : {12'd0, ua};

  always_comb begin
    q = '0;
    if (mode == 2'd3) q = ua / (ub | 12'd1);
  end
endmodule
