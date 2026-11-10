// mformal test: a FIFO with a memory, in a hierarchy. tests/mformal.cmake has the expected verdicts.
module fifo #(parameter int W = 8, parameter int D = 4) (
  input  logic clk, input logic rst,
  input  logic push, input logic pop, input logic [W-1:0] din,
  output logic [W-1:0] dout, output logic full, output logic empty);
  localparam int A = $clog2(D);
  logic [W-1:0] mem [D];
  logic [A:0] wp, rp;
  wire [A:0] count = wp - rp;
  assign full = count == D[A:0];
  assign empty = count == 0;
  assign dout = mem[rp[A-1:0]];
  always_ff @(posedge clk) begin
    if (rst) begin wp <= '0; rp <= '0; end
    else begin
      if (push && !full) begin mem[wp[A-1:0]] <= din; wp <= wp + 1'b1; end
      if (pop && !empty) rp <= rp + 1'b1;
    end
  end
  default clocking cb @(posedge clk); endclocking
  default disable iff (rst);
  a_count: assert property (count <= D);
  a_not_both: assert property (!(full && empty));
  a_full_stays: assert property (full && !pop |=> full);
  // true, but not inductive on its own: needs a_count as a lemma
  a_push_fills: assert property (push |=> !empty);
  // false: a push into a full FIFO is dropped
  a_push_counts: assert property (push && !pop |=> count == $past(count) + 1'b1);
  c_full: cover property (full);
  c_full_then_empty: cover property (full ##[1:$] empty);
endmodule
module top(input logic clk, input logic rst, input logic push, pop, input logic [7:0] din, output logic [7:0] dout, output logic full, empty);
  fifo #(.W(8), .D(4)) u (.*);
  a_top: assert property (@(posedge clk) disable iff (rst) u.full |-> !u.empty);
endmodule
