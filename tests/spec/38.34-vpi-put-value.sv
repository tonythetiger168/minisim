// IEEE 1800-2023 38.34, 38.35, 38.15: vpi_put_value writes a variable at
// once, one bit of a vector through vpi_handle_by_index, and another
// variable after a delay given in the module's time unit. Names resolve
// relative to the calling module. vpi_control(vpiFinish) ends the run.
//! args: data/vpi/putvalue.c
//! icarus-vpi: data/vpi/putvalue.c
`timescale 1ns/100ps
module top;
  sub u();
endmodule
module sub;
  reg [7:0] data = 0;
  reg [3:0] flags = 4'b0000;
  integer slow = 0;
  always @(slow) $display("%t slow=%0d", $realtime, slow);
  initial begin
    #3 $drive;
    $display("data=%h flags=%b slow=%0d", data, flags, slow);
    #5 $stop_here;
    $display("not reached");
  end
endmodule
//> scope top.u, unit -9, precision -10
//> now 3.0 units, 30 ticks
//> data=a5 flags=0100 slow=0
//>                   55 slow=77
//> stopping
