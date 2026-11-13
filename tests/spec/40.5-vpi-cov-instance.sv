// IEEE 1800-2023 40.5.1: vpi_control(vpiCoverageStop / vpiCoverageStart,
// type, instance) on an instance below the top. Its statement, toggle or
// FSM counts stay as they were at the stop until the start; other
// instances keep counting. vpiCoverageReset of a stopped instance zeroes
// it and leaves it stopped.
//! icarus-skip: Icarus 13 has no coverage API
//! args: -cov /tmp/msim-spec-40.5-vpi-cov-instance.json data/vpi/covinst.c
module cnt_m(input logic clk);
  logic [3:0] q = 0;
  always @(posedge clk) q <= q + 1;
endmodule
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  cnt_m u1(.clk), u2(.clk);
  initial begin
    #12 $show("start");
    $stop_u1;
    #40 $show("u1 stopped");
    $reset_u1;
    #20 $show("u1 reset");
    $start_u1;
    #40 $show("u1 started");
    $finish;
  end
endmodule
//> start: u1 1 u2 1
//> u1 stopped: u1 1 u2 3
//> u1 reset: u1 0 u2 3
//> u1 started: u1 3 u2 4
