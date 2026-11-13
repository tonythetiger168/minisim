// IEEE 1800-2023 40.5: the coverage API. vpi_get(<coverage type>, instance)
// counts the covered items of an instance (statements, toggled bits, FSM
// states, assertions); vpiCovered / vpiCoveredCount / vpiCoveredMax read
// one assertion, FSM, FSM state, net or bit; vpi_iterate(vpiFsm) and
// vpiFsmStates walk the FSMs; vpi_control starts, stops, resets, checks
// and saves coverage.
//! icarus-skip: Icarus 13 has no coverage API
//! args: -cov /tmp/msim-spec-40-vpi-coverage.json data/vpi/coverage.c
module fsm_m(input logic clk, input logic rst, input logic go);
  typedef enum logic [1:0] {IDLE, RUN, DONE, ERR} st_t;
  st_t state;
  logic [3:0] cnt = 0;
  always_ff @(posedge clk or posedge rst)
    if (rst) state <= IDLE;
    else case (state)
      IDLE: if (go) state <= RUN;
      RUN: state <= DONE;
      DONE: state <= IDLE;
      ERR: state <= IDLE;
    endcase
  always @(posedge clk)
    if (state == RUN) cnt <= cnt + 1;
    else if (state == ERR) cnt <= 0;
  a_go: assert property (@(posedge clk) disable iff (rst) go |=> state == RUN);
  c_done: cover property (@(posedge clk) state == DONE);
endmodule

module top;
  logic clk = 0, rst = 1, go = 0;
  always #5 clk = ~clk;
  fsm_m u(.clk, .rst, .go);
  // sv_vpi_user.h (annex M)
  localparam int vpiCoverageStart = 750, vpiCoverageStop = 751, vpiCoverageReset = 752;
  localparam int vpiAssertCoverage = 760, vpiStatementCoverage = 762, vpiToggleCoverage = 763;
  initial begin
    #12 rst = 0;
    repeat (2) begin
      @(negedge clk) go = 1;
      @(negedge clk) go = 0;
      @(negedge clk);
    end
    $cov_report;
    $cov_ctl(vpiCoverageStop, vpiToggleCoverage);
    $cov_ctl(vpiCoverageStop, vpiAssertCoverage);
    @(negedge clk) go = 1;
    @(negedge clk) go = 0;
    repeat (3) @(negedge clk);
    $cov_report;
    $cov_ctl(vpiCoverageStart, vpiToggleCoverage);
    $cov_ctl(vpiCoverageReset, vpiStatementCoverage);
    $cov_ctl(vpiCoverageReset, vpiAssertCoverage);
    $cov_report;
    $cov_save;
    $finish;
  end
endmodule
//> top.u: statements 9 toggles 5 fsm 3 asserts 2
//>   fsm state (top.u.state): covered 3 of 4, entered 6
//>     IDLE = 0: covered 1 count 2
//>     RUN = 1: covered 1 count 2
//>     DONE = 2: covered 1 count 2
//>     ERR = 3: covered 0 count 0
//>   cnt: 1 of 4 bits toggled, cnt[0]: 1
//>   a_go: covered 1, attempts 7 success 2 failure 0 vacuous 4 disabled 1 killed 0
//>   c_done: covered 1, attempts 7 success 1 failure 6 vacuous 0 disabled 0 killed 0
//>   check: statements 0 fsm 0
//> control 751 763: 1
//> control 751 760: 1
//> top.u: statements 9 toggles 5 fsm 3 asserts 2
//>   fsm state (top.u.state): covered 3 of 4, entered 10
//>     IDLE = 0: covered 1 count 4
//>     RUN = 1: covered 1 count 3
//>     DONE = 2: covered 1 count 3
//>     ERR = 3: covered 0 count 0
//>   cnt: 1 of 4 bits toggled, cnt[0]: 1
//>   a_go: covered 1, attempts 7 success 2 failure 0 vacuous 4 disabled 1 killed 0
//>   c_done: covered 1, attempts 7 success 1 failure 6 vacuous 0 disabled 0 killed 0
//>   check: statements 0 fsm 0
//> control 750 763: 1
//> control 752 762: 1
//> control 752 760: 1
//> top.u: statements 0 toggles 5 fsm 3 asserts 0
//>   fsm state (top.u.state): covered 3 of 4, entered 10
//>     IDLE = 0: covered 1 count 4
//>     RUN = 1: covered 1 count 3
//>     DONE = 2: covered 1 count 3
//>     ERR = 3: covered 0 count 0
//>   cnt: 1 of 4 bits toggled, cnt[0]: 1
//>   a_go: covered 0, attempts 0 success 0 failure 0 vacuous 0 disabled 0 killed 0
//>   c_done: covered 0, attempts 0 success 0 failure 0 vacuous 0 disabled 0 killed 0
//>   check: statements 0 fsm 0
//> save: 1
//> saved database: msim-cov
//> merge: 0
//> stop under a sub-instance: 1
