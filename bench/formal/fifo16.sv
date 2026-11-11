module fifo16 #(parameter W=8, D=16, A=4)(input logic clk, rst, push, pop, input logic [W-1:0] din, output logic [W-1:0] dout, output logic full, empty);
  logic [W-1:0] mem [D];
  logic [A:0] wp, rp;
  wire [A:0] cnt = wp - rp;
  assign full = cnt == D, empty = cnt == 0;
  assign dout = mem[rp[A-1:0]];
  always_ff @(posedge clk) if (rst) begin wp <= 0; rp <= 0; end else begin
    if (push && !full) begin mem[wp[A-1:0]] <= din; wp <= wp + 1'b1; end
    if (pop && !empty) rp <= rp + 1'b1;
  end
  a_cnt: assert property (@(posedge clk) disable iff (rst) cnt <= D);
  a_nofe: assert property (@(posedge clk) disable iff (rst) !(full && empty));
  a_full: assert property (@(posedge clk) disable iff (rst) full && !pop |=> full);
  c_full: cover property (@(posedge clk) disable iff (rst) full);
  // data integrity with a symbolic watched slot
  logic [A-1:0] slot; logic [W-1:0] d; logic seen;
  always_ff @(posedge clk) if (rst) seen <= 0; else if (push && !full && wp[A-1:0] == slot && !seen) begin seen <= 1; d <= din; end
     else if (pop && !empty && rp[A-1:0] == slot) seen <= 0;
  m_slot: assume property (@(posedge clk) $stable(slot));
  a_data: assert property (@(posedge clk) disable iff (rst) seen && !empty && rp[A-1:0] == slot |-> dout == d);
endmodule
