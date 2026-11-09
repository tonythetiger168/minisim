// Corner cases for msim -compile's cycle domain (codegen.hpp cycleDomain):
// clocked processes that share one event list run as one function, and the
// static combinational logic is re-evaluated after their NBAs. Everything a
// testbench can observe must stay the same as in the event-driven kernel.
module cycle;
  logic clk = 0, clk2 = 0, rst;
  logic [7:0] cnt, acc, sh, other;
  logic [15:0] wide_part;
  wire  [7:0] nxt = cnt + 8'd1;          // comb feeding a clocked process
  wire  [7:0] mix = acc ^ sh;            // comb fed by the domain
  wire        gclk = cnt[1];             // a domain reg used as a clock
  logic [7:0] slow;
  logic [3:0] hz;

  always #5 clk = ~clk;
  always #7 clk2 = ~clk2;                // a second clock: its own process, not in the domain

  always @(posedge clk) begin
    if (rst) cnt <= 0;
    else cnt <= nxt;
  end
  always @(posedge clk) begin
    if (rst) acc <= 8'h11;
    else if (cnt[0]) acc <= acc + mix;
    else acc <= acc - 8'd3;
  end
  always @(posedge clk) begin
    sh <= {sh[6:0], sh[7] ^ sh[3]};
    wide_part[11:4] <= cnt;              // partial NBA
  end
  always @(posedge clk2) other <= mix;   // different event list
  always @(posedge gclk) slow <= slow + 1;
  always @(posedge clk) hz <= rst ? 4'bz0x1 : {hz[2:0], hz[3]};   // X and Z through the domain

  initial begin
    rst = 1; sh = 8'h5a; slow = 0; wide_part = 16'hf00f;
    @(posedge clk); @(posedge clk); #1 rst = 0;
    @(acc);                              // wait on a domain reg
    $display("acc changed: %h", acc);
    wait (cnt == 8'd6);
    #1 $display("cnt=6 mix=%h", mix);
    @(negedge clk) $display("negedge: cnt=%0d acc=%h sh=%h", cnt, acc, sh);
    $monitor("monitor cnt=%0d mix=%h", cnt, mix);
    #140;
    $display("end: cnt=%0d acc=%h sh=%h other=%h slow=%0d wide=%h hz=%b", cnt, acc, sh, other, slow, wide_part, hz);
    $finish;
  end
endmodule
