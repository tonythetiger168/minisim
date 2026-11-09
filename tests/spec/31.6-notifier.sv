`timescale 1ns/100ps
// IEEE 1800-2023 31.6: each violation toggles the notifier: x becomes 1
// (either 0 or 1 is allowed), then 0, 1, ...; z stays z. Limits are in the module's time unit, and the
// report gives times in that unit.
//! icarus-skip: Icarus 13 does not run timing checks
module dff(input clk, input d);
  reg notifier;
  reg nz = 1'bz;
  always @(notifier) $display("%0d notifier=%b", $time, notifier);
  specify
    $setup(d, posedge clk, 1.5, notifier);
    $hold(posedge clk, d, 1, nz);
  endspecify
endmodule
module top;
  reg clk = 0, d = 0;
  dff u(clk, d);
  always #5 clk = ~clk;
  initial begin
    #4 d = 1;       // 1 before the edge at 5
    #4 d = 0;     // 8: fine
    #6.5 d = 1;     // 14.5: 0.5 before 15
    #1 d = 0;       // 15.5: hold, notifier z
    #9.5 d = 1;     // 25: 0 before: fine; at 25 hold with nz
    #10 $display("nz=%b", u.nz);
    $finish;
  end
endmodule

//> "31.6-notifier.sv", 11: Timing violation in top.u
//>     $setup( d:4, posedge clk:5, 1.5 );
//> 5 notifier=1
//> "31.6-notifier.sv", 11: Timing violation in top.u
//>     $setup( d:14.5, posedge clk:15, 1.5 );
//> 15 notifier=0
//> "31.6-notifier.sv", 12: Timing violation in top.u
//>     $hold( posedge clk:15, d:15.5, 1 );
//> "31.6-notifier.sv", 12: Timing violation in top.u
//>     $hold( posedge clk:25, d:25, 1 );
//> nz=z
