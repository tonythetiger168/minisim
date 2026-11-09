// IEEE 1800-2023 38.36: VPI callbacks. A value-change callback reports the
// object, the time and the new value; it stays until vpi_remove_cb. An
// after-delay callback runs at its time and may write signals, which
// processes then see. A read-only synch callback runs at the end of the
// time slot, after the design has settled.
//! args: data/vpi/callbacks.c
//! icarus-vpi: data/vpi/callbacks.c
module top;
  reg clk = 0;
  reg [3:0] count = 0;
  wire [3:0] bus = count + 1;
  reg [7:0] mem [0:3];
  always #5 clk = ~clk;
  always @(posedge clk) count <= count + 1;
  always @(count) if (count == 9) $display("%0t: design saw count=9", $time);
  initial begin
    #12 mem[1] = 8'h3c;
    #30 $finish;
  end
endmodule
//> start of simulation
//> 0: top.bus -> 0001
//> 5: top.clk -> 1
//> 5: top.bus -> 0010
//> 10: top.clk -> 0
//> 12: top.mem[1] -> 00111100
//> 15: next simulation time
//> 15: top.clk -> 1
//> 15: top.bus -> 0011
//> 17: after delay: writing 9
//> 17: top.bus -> 1010
//> 17: design saw count=9
//> 17: read-only synch: count=9
//> 20: top.clk -> 0
//> removed the clk callback
//> 25: top.bus -> 1011
//> 35: top.bus -> 1100
//> 42: end of simulation
