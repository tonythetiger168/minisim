// IEEE 1800-2023 15.5.2: ->> triggers the event in the NBA region, after #0
// code in the same time slot (15.5.2).
//! icarus-skip: Icarus 13 does not support ->>
module top;
  event e;
  int v = 0;
  initial begin
    fork
      begin @(e); $display("v=%0d", v); end
      begin #1; ->>e; #0 v = 5; end
    join
  end
endmodule

//> v=5
