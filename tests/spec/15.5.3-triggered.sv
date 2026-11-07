//! icarus-skip: Icarus 13 has no .triggered and no ->>
// IEEE 1800-2023 15.5.3: e.triggered is true in the time slot e fires, so a
// wait on it does not miss a trigger that ran first. 15.5.1: ->> e fires in NBA.
module top;
  event e, f;
  initial begin
    #5 -> e;
    ->> f;
    $display("%0d after ->> f: %0d", $time, f.triggered);
  end
  initial begin
    #5 wait (e.triggered) $display("%0d e.triggered", $time);
    @f $display("%0d got f", $time);
  end
  initial #6 $display("%0d e.triggered later: %0d", $time, e.triggered);
endmodule

//> 5 after ->> f: 0
//> 5 e.triggered
//> 5 got f
//> 6 e.triggered later: 0
