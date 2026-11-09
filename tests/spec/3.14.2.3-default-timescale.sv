// IEEE 1800-2023 3.14.2.3: a design element with no `timescale or timeunit
// takes the tool's default. Like VCS and Xcelium run with -timescale, msim
// uses 1ns/1ps when other elements of the design have a timescale.
// Also checks that a name used before its declaration is accepted.
//! icarus-skip: Icarus uses a 1s default and does not allow use before declaration
module other;
  initial #2 $display("other %0t %0d", $realtime, $time);
endmodule
`timescale 1ns/1ps
module top;
  other u();
  initial begin
    #1.5 later = 3;
    $display("top %0t %0d", $realtime, later);
  end
  int later;
endmodule
//> top 1500 3
//> other 2000 2
