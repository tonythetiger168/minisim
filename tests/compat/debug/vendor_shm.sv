// Xcelium SHM tasks on msim: the waveform is written as FST (vendor_tasks.hpp).
module tb;
  logic clk = 0; logic [3:0] c = 0;
  always #5 clk = ~clk;
  always @(posedge clk) c <= c + 1;
  initial begin
    $shm_open("waves.shm");
    $shm_probe(tb, "AS");
    #30 $shm_close; $finish;
  end
endmodule
