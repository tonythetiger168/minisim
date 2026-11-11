// Don't-cares a synthesis tool uses: (* full_case *) with a value left out,
// a register that only ever loads one constant, and one never given a value.
module dontcare(input logic clk, input logic [1:0] sel, input logic [3:0] a, b, c,
                output logic [3:0] y, output logic [3:0] t, output logic v);
`ifdef FOLDED
  always_comb
    case (sel)
      2'd0: y = a;
      2'd1: y = b;
`ifdef BUG
      default: y = a;
`else
      default: y = c;   // also for 3, which the reference leaves out
`endif
    endcase
  assign t = '0;
  assign v = 1'b0;
`else
  logic [3:0] never;   // no assignment gives it a value
  logic       zero;    // only ever loads 0
  always @* begin
    (* full_case *)
    case (sel)
      2'd0: y = a;
      2'd1: y = b;
      2'd2: y = c;
    endcase
  end
  always_ff @(posedge clk) begin
    zero <= 1'b0;
    if (sel[0]) never <= 'x;
  end
  assign t = never;
  assign v = zero;
`endif
endmodule
