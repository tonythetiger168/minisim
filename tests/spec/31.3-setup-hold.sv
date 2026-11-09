`timescale 1ns/1ns
// IEEE 1800-2023 31.3.1, 31.3.2: $setup and $hold. A data change at the
// same time as the clock edge violates $hold but not $setup. A notifier
// toggles on each violation; +notimingcheck turns the checks off.
//! icarus-skip: Icarus 13 does not run timing checks
module dff(input clk, input d, output reg q);
  reg notifier;
  always @(posedge clk) q <= d;
  specify
    $setup(d, posedge clk, 3, notifier);
    $hold(posedge clk, d, 2, notifier);
  endspecify
endmodule
module top;
  reg clk = 0, d = 0;
  wire q;
  dff u(clk, d, q);
  always #10 clk = ~clk;
  initial begin
    #8 d = 1;     // 2 before the edge at 10: setup violation
    #3 d = 0;     // 1 after the edge at 10: hold violation
    #15 d = 1;    // 26: fine (edge at 30)
    #6 d = 0;     // 32: the hold window from 30 ends at 32: fine
    #18 d = 1;    // 50: same time as the edge: setup no, hold yes
    #10 $display("notifier=%b", u.notifier);
    $finish;
  end
endmodule

//> "31.3-setup-hold.sv", 10: Timing violation in top.u
//>     $setup( d:8, posedge clk:10, 3 );
//> "31.3-setup-hold.sv", 11: Timing violation in top.u
//>     $hold( posedge clk:10, d:11, 2 );
//> "31.3-setup-hold.sv", 11: Timing violation in top.u
//>     $hold( posedge clk:50, d:50, 2 );
//> notifier=1
