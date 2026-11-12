// Reference fixture for Verilator. Native Minisim does not parse SVA statements yet.
module coverage_fsm #(parameter bit BUG = 0)
  (input logic clk, rst, start, output logic [1:0] state, output logic [3:0] cnt);
  localparam logic [1:0] IDLE = 0, RUN = 1, DONE = 2;
  always_ff @(posedge clk) begin
    if (rst) begin state <= IDLE; cnt <= 0; end
    else case (state)
      IDLE: if (start) begin state <= RUN; cnt <= 0; end
      RUN: if (cnt == 3) state <= BUG ? IDLE : DONE;
           else cnt <= cnt + 1;
      DONE: if (!start) state <= IDLE;
      default: state <= IDLE;
    endcase
  end

  ap_legal: assert property (@(posedge clk) disable iff (rst) state != 3);
  ap_run_done: assert property (@(posedge clk) disable iff (rst)
      (state == RUN && cnt == 3) |=> state == DONE);

  cp_state_idle: cover property (@(posedge clk) disable iff (rst) state == IDLE);
  cp_state_run: cover property (@(posedge clk) disable iff (rst) state == RUN);
  cp_state_done: cover property (@(posedge clk) disable iff (rst) state == DONE);
  cp_idle_run: cover property (@(posedge clk) disable iff (rst)
      $past(!rst) && $past(state) == IDLE && $past(start) && state == RUN);
  cp_run_done: cover property (@(posedge clk) disable iff (rst)
      $past(!rst) && $past(state) == RUN && $past(cnt) == 3 && state == DONE);
  cp_done_idle: cover property (@(posedge clk) disable iff (rst)
      $past(!rst) && $past(state) == DONE && !$past(start) && state == IDLE);
endmodule
