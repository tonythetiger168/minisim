// IEEE 1800-2023 20.10: $info, $warning, $error print a message with the
// severity, file, line, scope and time; $fatal also ends the run with an
// error (20.10).
//! icarus-skip: the message format is tool-specific
//! exit: 1
module top;
  initial begin
    #2 $info("i=%0d", 1);
    $warning("w");
    $error;
    $display("still running");
    $fatal(1, "bye");
    $display("not reached");
  end
endmodule

//> Info: "20.10-severity-tasks.sv", 8: top: at time 2
//> i=1
//> Warning: "20.10-severity-tasks.sv", 9: top: at time 2
//> w
//> Error: "20.10-severity-tasks.sv", 10: top: at time 2
//> still running
//> Fatal: "20.10-severity-tasks.sv", 12: top: at time 2
//> bye
