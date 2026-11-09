// IEEE 1800-2023 6.17: Assigning one event to another makes both names the
// same event (6.17).
//! xfail: event assignment does not alias
//! icarus-skip: Icarus 13 does not alias events
module top;
  event e1, e2;
  initial begin
    e2 = e1;
    fork
      begin @(e2); $display("e2 woke at %0t", $time); end
      #3 ->e1;
    join
  end
endmodule

//> e2 woke at 3
