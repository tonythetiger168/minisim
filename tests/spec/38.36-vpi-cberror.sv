// IEEE 1800-2023 38.36.3: cbPLIError is called when a VPI routine reports
// an error (and not again for an error inside the callback), and stops
// after vpi_remove_cb; cbError is called for each run-time error of the
// simulation ($error, a failed immediate assertion, $fatal), with
// vpi_chk_error giving the message.
//! icarus-skip: Icarus 13 does not call cbError / cbPLIError
//! args: data/vpi/errors.c
//! exit: 1
module top;
  int x = 1;
  initial begin
    $errs;
    #2 $error("bad value %0d", x);
    #1 assert (x == 2) else $error("x is %0d", x);
    #1 $fatal(1, "stop");
  end
endmodule
//> cbPLIError at 0: level 3: vpi_handle: null reference handle
//> after remove: chk_error 3
//> Error: "38.36-vpi-cberror.sv", 13: top: at time 2
//> bad value 1
//> cbError at 2: level 3: Error: "38.36-vpi-cberror.sv", 13: top
//> Error: "38.36-vpi-cberror.sv", 14: top: at time 3
//> x is 1
//> cbError at 3: level 3: Error: "38.36-vpi-cberror.sv", 14: top
//> Fatal: "38.36-vpi-cberror.sv", 15: top: at time 4
//> stop
//> cbError at 4: level 3: Fatal: "38.36-vpi-cberror.sv", 15: top
