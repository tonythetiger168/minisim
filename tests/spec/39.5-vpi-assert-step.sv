// IEEE 1800-2023 39.5.2: vpi_control(vpiAssertionEnableStep, assertion,
// attempt start time, vpiAssertionClockSteps) makes each clock step of
// that attempt call cbAssertionStepSuccess (the attempt goes on or
// matches) or cbAssertionStepFailure (it fails), with the attempt's start
// time, the states it went from and to, and the expressions that matched in
// the step (matched_exprs); on the failure, failExpr is the expression that
// did not hold. It may name an attempt that has
// not started yet. vpiAssertionDisableStep (assertion, start time) stops
// it. vpiAssertionKill (assertion, start time) discards that attempt only:
// no outcome, cbAssertionKill with no attempt information, and the
// assertion stays enabled. An immediate assertion has no steps. Assertion
// system control (39.5.1) takes a scope handle or NULL.
//! icarus-skip: Icarus 13 has no assertion API
//! args: data/vpi/assertstep.c
module top;
  logic clk = 0, req = 0, ack = 0;
  always #5 clk = ~clk;
  p: assert property (@(posedge clk) req |-> ##[1:2] ack) else $display("%0t: p failed", $time);
  initial begin
    imm: assert (1);
    $step_on;
    req = 1;             // attempt at 5: ack at 15 passes
    #10 req = 0; ack = 1;
    #10 ack = 0; req = 1; // attempt at 25: no ack by 45, fails
    #10 req = 0;
    #30 $step_off;
    req = 1;             // attempt at 65: stepping off again, then killed
    #10 req = 0;
    $kill_attempt;
    #10 req = 1;         // attempt at 85: still enabled, ack at 95 passes
    #10 req = 0; ack = 1;
    #10 $finish;
  end
endmodule
//> enable step 5: 1
//> enable step 25: 1
//> enable step 65: 1
//> immediate: vpi_control: only a concurrent assertion has clock steps
//> no step control: vpi_control: vpiAssertionEnableStep needs vpiAssertionClockSteps
//> system control on an assertion: vpi_control: assertion system control takes a scope handle or NULL
//> 5: step success, attempt from 5, state 0 -> 1
//>   matched req
//> 15: step success, attempt from 5, state 1 -> 2
//>   matched ack
//> 15: success, attempt from 5
//> 25: step success, attempt from 25, state 0 -> 1
//>   matched req
//> 35: step success, attempt from 25, state 1 -> 3
//> 45: step failure, attempt from 25, state 3 -> 4
//> 45: failure, attempt from 25
//>   failExpr ack
//> 45: p failed
//> disable step 65: 1
//> 70: kill
//> kill 65: 1
//> 95: success, attempt from 85
