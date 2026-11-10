// mformal test: proofs that need an invariant, and a failure deeper than -depth (PDR).
// tests/mformal.cmake has the expected verdicts.
module inv(input logic clk, input logic rst, input logic en, input logic [7:0] d);
  logic [7:0] a, b;
  always_ff @(posedge clk) if (rst) begin a <= 8'd0; b <= 8'd0; end else if (en) begin a <= a + 8'd1; b <= b + 8'd1; end
  a_same: assert property (@(posedge clk) disable iff (rst) a[3:0] == b[3:0]);
  logic [7:0] ring;
  always_ff @(posedge clk) if (rst) ring <= 8'd1; else if (en) ring <= {ring[6:0], ring[7]};
  a_ring: assert property (@(posedge clk) disable iff (rst) !(ring[0] && ring[5]));
  logic [5:0] c;
  always_ff @(posedge clk) if (rst) c <= 6'd0; else if (c != 6'd40) c <= c + 6'd1;
  a_c: assert property (@(posedge clk) disable iff (rst) c != 6'd50);
  a_deep: assert property (@(posedge clk) disable iff (rst) c != 6'd35);
  logic [1:0] st;
  logic [7:0] acc;
  always_ff @(posedge clk) if (rst) begin st <= 0; acc <= 0; end else begin
    case (st) 0: begin acc <= 0; if (en) st <= 1; end 1: begin acc <= acc + 8'd3; if (acc >= 8'd30) st <= 2; end 2: st <= 0; default: st <= 0; endcase end
  a_acc: assert property (@(posedge clk) disable iff (rst) acc <= 8'd33);
endmodule
