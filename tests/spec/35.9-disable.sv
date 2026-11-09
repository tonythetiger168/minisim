// IEEE 1800-2023 35.9: Disabling a process while its import task waits in an
// exported task: the exported task returns 1, svIsDisabledState is true, and
// the import task returns 1 after svAckDisabledState.
//! args: data/dpi/tasks.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  import "DPI-C" context task long_job();
  export "DPI-C" task sv_wait;
  logic clk = 0;
  always #5 clk = ~clk;
  task sv_wait(input int cycles);
    repeat (cycles) @(posedge clk);
  endtask
  initial begin : job
    long_job();
    $display("not reached");
  end
  initial begin
    #32 disable job;
    $display("disabled at %0t", $time);
    #20 $display("end at %0t", $time);
    $finish;
  end
endmodule

//> C: disabled after 3 waits, state 1
//> disabled at 32
//> end at 52
