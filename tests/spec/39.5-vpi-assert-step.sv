// IEEE 1800-2023 39.5.2: vpi_control(vpiAssertionEnableStep, assertion)
// makes each clock step of each attempt call cbAssertionStepSuccess (the
// attempt goes on or matches) or cbAssertionStepFailure (it fails), with
// the attempt's start time and the states it went from and to;
// vpiAssertionDisableStep stops it. An immediate assertion has no steps.
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
    req = 1;             // attempt at 65: no step callbacks
    #10 req = 0; ack = 1;
    #20 $finish;
  end
endmodule
//> enable step: 1
//> immediate: vpi_control: only a concurrent assertion has clock steps
//> 5: step success, attempt from 5, state 0 -> 1
//> 15: step success, attempt from 5, state 1 -> 2
//> 15: success, attempt from 5
//> 15: step success, attempt from 15, state 0 -> 2
//> 25: step success, attempt from 25, state 0 -> 1
//> 35: step success, attempt from 25, state 1 -> 3
//> 35: step success, attempt from 35, state 0 -> 2
//> 45: step failure, attempt from 25, state 3 -> 4
//> 45: failure, attempt from 25
//> 45: step success, attempt from 45, state 0 -> 2
//> 45: p failed
//> 55: step success, attempt from 55, state 0 -> 2
//> disable step: 1
//> 75: success, attempt from 65
