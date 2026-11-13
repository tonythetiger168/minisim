// Verdi FSDB tasks on msim: the waveform is written as FST (vendor_tasks.hpp).
module sub(input logic clk, output logic [3:0] q);
  initial q = 0;
  always @(posedge clk) q <= q + 1;
endmodule
module tb;
  logic clk = 0; logic [3:0] c = 0; logic [7:0] mem [0:1];
  sub u1(.clk, .q());
  always #5 clk = ~clk;
  always @(posedge clk) c <= c + 1;
  initial begin
    mem[0] = 8'h11;
    $fsdbDumpfile("wave.fsdb");
    $fsdbDumpvars(0, tb);
    $fsdbDumpvars(0, "tb.u1", "+all");
    $fsdbDumpMDA();
    $fsdbDumpSVA();
    #20 $fsdbDumpoff;
    #20 $fsdbDumpon;
    #10 $fsdbDumpflush; $finish;
  end
endmodule
