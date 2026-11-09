// IEEE 1800-2023 14.4, 14.5, 14.11, 14.16: default and per-signal output
// skews, an inout on a net (driven through its own driver, 14.16), an output
// that is a concatenation driven one slice at a time, and ##0 after the
// clocking event (no wait).
//! icarus-skip: Icarus 13 does not support clocking blocks
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  wire [7:0] bus;
  logic [7:0] r = 0;
  logic [3:0] hi4, lo4;
  default clocking cb @(posedge clk);
    default output #2;
    inout bus;
    output nib = {hi4, lo4};
    output #0 r;
  endclocking
  always @(bus) $display("%0t bus=%h", $time, bus);
  always @(hi4 or lo4) $display("%0t hi4=%h lo4=%h", $time, hi4, lo4);
  initial begin
    ##1;
    ##0;                    // the event already happened at 5: no wait
    $display("%0t after ##0", $time);
    cb.bus <= 8'ha5;        // 5 + 2
    cb.nib[3:0] <= 4'h3;    // lo4 at 7
    cb.r <= 8'd9;           // re-NBA at 5
    $display("%0t r=%0d", $time, r);
    @(cb);
    $display("%0t cb.bus=%h r=%0d", $time, cb.bus, r);
    cb.nib <= 8'hf0;        // 17
    #1 cb.bus <= 8'hzz;     // at 16: the next event (25) + 2
  end
  initial #40 $finish;
endmodule

//> 5 after ##0
//> 5 r=0
//> 7 hi4=x lo4=3
//> 7 bus=a5
//> 15 cb.bus=a5 r=9
//> 17 hi4=f lo4=0
//> 27 bus=zz
