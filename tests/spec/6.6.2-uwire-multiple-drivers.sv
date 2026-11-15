// IEEE 1800-2023 6.6.2: a uwire net may have only one driver; a second
// driver is an error, found before the simulation starts.
//! icarus-skip: Icarus rejects it at compile time too, which the harness cannot check
//! exit: 1
module top;
  uwire two;
  assign two = 1'b1;
  assign two = 1'b0;
  initial $display("not reached");
endmodule
