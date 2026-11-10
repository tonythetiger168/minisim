// mformal test: sequence and property operators on a request / grant handshake
// whose environment is constrained by assumptions; no reset, declared initial values.
module seq(input logic clk, input logic req, input logic [1:0] dly, output logic gnt);
  logic [1:0] t = 2'd0;
  logic busy = 1'b0;
  always_ff @(posedge clk) begin
    gnt <= 1'b0;
    if (!busy && req) begin busy <= 1'b1; t <= dly; end
    else if (busy) begin
      if (t == 2'd0) begin gnt <= 1'b1; busy <= 1'b0; end
      else t <= t - 2'd1;
    end
  end
  initial gnt = 1'b0;
  default clocking @(posedge clk); endclocking
  m_hold: assume property (req && !gnt |=> req || gnt);       // req is held until granted
  m_drop: assume property (gnt |-> !req);                     // and dropped with the grant
  a_window: assert property ($rose(req) |-> ##[1:5] gnt);     // granted within 5 cycles
  a_tight: assert property ($rose(req) |-> ##[1:3] gnt);      // false: dly = 3 takes 5
  a_once: assert property (gnt |=> !gnt);
  a_rep: assert property (not (busy [*6]));                 // never busy 6 cycles in a row
  a_thr: assert property ($rose(busy) |-> busy throughout (t == 2'd0) [->1]);
  a_goto: assert property ($rose(req) |-> !gnt [*1:5] ##1 gnt);
  a_nexttime: assert property (gnt |-> nexttime !busy);
  a_iff: assert property (@(posedge clk iff busy) t != 2'd0 |=> t == $past(t) - 2'd1);
  c_slow: cover property (req ##1 !gnt [*4] ##1 gnt);
  c_fast: cover property ($rose(req) ##2 gnt);
  c_never: cover property (gnt ##1 gnt);
  c_seq: cover sequence (gnt ##[1:3] req);
endmodule
