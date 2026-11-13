// 40 small cores, each on its own gated clock: the compiled cycle domain
// has one clock group per core. A batch of 32 or more clocked processes
// runs on several threads, but the groups share the domain's NBA slots,
// so they must run one at a time (Sim::runSeqs). Every thread count must
// print the same line.
module mgcore #(parameter int ID = 0) (input logic clk, input logic run, output logic [15:0] sig);
  logic gclk;
  assign gclk = clk & run;
  logic [15:0] acc = 16'(ID * 37 + 1);
  logic [15:0] mem [0:7];
  logic [2:0] p = 0;
  initial for (int k = 0; k < 8; k++) mem[k] = 16'(k + ID);
  always @(posedge gclk) begin
    acc <= {acc[14:0], acc[15] ^ acc[13]} + mem[p];
    mem[p] <= acc ^ 16'(ID);
    p <= p + 3'd1;
  end
  assign sig = acc ^ mem[0];
endmodule

module manygroups;
  logic clk = 0;
  logic run = 0;
  logic [15:0] sig [0:39];
  always #5 clk = ~clk;
  for (genvar i = 0; i < 40; i++) begin : g
    mgcore #(i) u (clk, run, sig[i]);
  end
  initial begin
    logic [15:0] x;
    #12 run = 1;
    #20000;   // long enough for the thread tuner (Sim::Tuner) to try 2 and 4 threads
    x = 0;
    for (int i = 0; i < 40; i++) x = {x[14:0], x[15]} ^ sig[i];
    $display("sum=%h s0=%h s39=%h", x, sig[0], sig[39]);
    $finish;
  end
endmodule
