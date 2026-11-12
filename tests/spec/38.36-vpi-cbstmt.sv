// IEEE 1800-2023 38.36.1: cbStmt calls back each time its statement runs,
// with the statement handle as obj; vpi_remove_cb stops it.
//! icarus-skip: Icarus 13 has no cbStmt
//! args: data/vpi/cbstmt.c
module top;
  int n = 0;
  initial begin
    $arm;
    repeat (3) begin
      #2
        n = n + 1;   // its own line: a callback is found by the statement's line
      $display("n %0d", n);
    end
    $disarm;
    #2 n = n + 1;
    $display("n %0d", n);
  end
endmodule
//> @2 cbStmt: vpiAssignment line 11
//> n 1
//> @4 cbStmt: vpiAssignment line 11
//> n 2
//> @6 cbStmt: vpiAssignment line 11
//> n 3
//> n 4
