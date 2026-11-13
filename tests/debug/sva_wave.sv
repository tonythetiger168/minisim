// Assertions in the waveform (msim -vcd-sva, $fsdbDumpSVA): a_gnt fails at
// 45 and 85 (gnt is held low after a request), c_req covers each request.
module arb(input logic clk, input logic req, input logic hold, output logic gnt);
  always_ff @(posedge clk) gnt <= req && !hold;
  a_gnt: assert property (@(posedge clk) req |=> gnt) else $display("%0t a_gnt failed", $time);
  c_req: cover property (@(posedge clk) req);
endmodule
module tb;
  logic clk = 0, req = 0, hold = 0, gnt;
  arb u(.clk, .req, .hold, .gnt);
  always #5 clk = ~clk;
  initial begin
    $fsdbDumpfile("sva.fsdb");
    $fsdbDumpvars(0, tb);
    $fsdbDumpSVA;
    @(negedge clk) req = 1;
    @(negedge clk) req = 0;
    @(negedge clk) req = 1; hold = 1;
    @(negedge clk) req = 0; hold = 0;
    @(negedge clk) req = 1;
    @(negedge clk) req = 0;
    @(negedge clk) req = 1; hold = 1;
    @(negedge clk) req = 0; hold = 0;
    #30 $finish;
  end
endmodule
