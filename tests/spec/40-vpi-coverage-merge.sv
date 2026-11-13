// IEEE 1800-2023 40.5.1: vpi_control(vpiCoverageMerge, type, name) adds the
// counts of a saved coverage database to the current ones, one coverage
// type at a time. Saved, reset, then merged, the statement, toggle, FSM
// and assertion coverage come back; merged again, the FSM state and
// assertion counts double. A missing database is an error.
//! icarus-skip: Icarus 13 has no coverage API
//! args: -cov /tmp/msim-spec-40-vpi-coverage-merge.json data/vpi/covmerge.c
module top;
  typedef enum logic [1:0] {IDLE, RUN, DONE} st_t;
  st_t state = IDLE;
  logic clk = 0;
  logic [1:0] cnt = 0;
  always #1 clk = ~clk;
  always @(posedge clk) begin
    cnt <= cnt + 1;
    case (state)
      IDLE: state <= RUN;
      RUN: if (cnt == 2) state <= DONE;
      DONE: state <= IDLE;
    endcase
  end
  chk: assert property (@(posedge clk) state != 2'b11);
  initial begin
    #20 $merge;
    $finish;
  end
endmodule
//> run: statements 8 toggles 5 fsm 3 assertions 1
//>   states: IDLE=3 RUN=3 DONE=2
//>   chk attempts 10 successes 10
//> save 1
//> reset: statements 0 toggles 0 fsm 0 assertions 0
//>   states: IDLE=0 RUN=0 DONE=0
//>   chk attempts 0 successes 0
//> merge 762: 1
//> merge 763: 1
//> merge 761: 1
//> merge 760: 1
//> merged: statements 8 toggles 5 fsm 3 assertions 1
//>   states: IDLE=3 RUN=3 DONE=2
//>   chk attempts 10 successes 10
//> merged twice (fsm, assertions): statements 8 toggles 5 fsm 3 assertions 1
//>   states: IDLE=6 RUN=6 DONE=4
//>   chk attempts 20 successes 20
//> missing database: vpi_control: cannot read the coverage database /tmp/msim-spec-40-vpi-coverage-merge-saved.json
