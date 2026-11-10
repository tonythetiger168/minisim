// IEEE 1800-2023 16.4: A deferred assertion reports once, after glitches in
// the time slot settle (16.4).
//! icarus-skip: Icarus 13 does not support deferred assertions
module top;
  logic a = 0;
  always_comb assert #0 (a == 0 || a == 1) else $display("bad");
  initial begin
    a = 1; a = 0; a = 1;
    #1 $display("done %b", a);
  end
endmodule

//> done 1
