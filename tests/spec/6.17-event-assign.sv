// IEEE 1800-2023 6.17 / 15.5.5: assigning one event to another makes both
// names the same event. A process already waiting on the old event is not
// woken by the merged one (15.5.5.1). An initializer aliases too, and an
// event set to null is never triggered (15.5.5.2).
//! icarus-skip: Icarus 13 does not alias events
module top;
  event e1, e2, e3;
  event e4 = e1;
  event en;
  initial begin
    fork
      begin @(e2); $display("old e2 woke at %0t", $time); end   // waits on the old e2
      begin #1 e2 = e1; end
      begin #2 @(e2); $display("e2 woke at %0t trig=%0d", $time, e1.triggered); end
      begin #2 @(e4); $display("e4 woke at %0t", $time); end
      begin #3 ->e2; end
      begin #4 e3 = e2; @(e1); $display("e1 woke at %0t via e3", $time); end
      begin #5 ->e3; end
      begin #6 en = null; fork @(en); join_none #1 ->en; $display("null done at %0t", $time); end
    join
  end
endmodule
//> e2 woke at 3 trig=1
//> e4 woke at 3
//> e1 woke at 5 via e3
//> null done at 7
