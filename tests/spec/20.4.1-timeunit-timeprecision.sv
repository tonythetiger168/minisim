//! icarus-skip: Icarus has no $timeunit or $timeprecision
// IEEE 1800-2023 20.4.1: $timeunit and $timeprecision return the time unit
// and precision of a design element as a power of ten (Table 20-2): of the
// current scope with no argument, of a named instance, of the compilation
// unit with $unit, and the simulation precision with $root.
timeunit 1us; timeprecision 10ns;
module sub;
  timeunit 100ps; timeprecision 1fs;
  initial $display("sub %0d %0d", $timeunit, $timeprecision());
endmodule
module top;
  timeunit 1ns; timeprecision 1ps;
  sub s();
  int u, p;
  initial begin
    u = $timeunit; p = $timeprecision;
    $display("top %0d %0d", u, p);
    $display("top() %0d %0d", $timeunit(), $timeprecision());
    $display("s %0d %0d", $timeunit(s), $timeprecision(s));
    $display("unit %0d %0d", $timeunit($unit), $timeprecision($unit));
    $display("root %0d %0d", $timeunit($root), $timeprecision($root));
  end
endmodule
//> sub -10 -15
//> top -9 -12
//> top() -9 -12
//> s -10 -15
//> unit -6 -8
//> root -15 -15
