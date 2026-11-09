// IEEE 1800-2023 35.8: An import task may call exported tasks that consume
// time; the calling process waits while the C code is suspended, and other
// processes keep running (35.8, 35.5.2).
//! args: data/dpi/tasks.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  import "DPI-C" context task run_seq(input int n);
  export "DPI-C" task sv_wait;
  export "DPI-C" task sv_drive;
  export "DPI-C" function sv_now;
  logic clk = 0;
  int data = 0;
  always #5 clk = ~clk;
  task sv_wait(input int cycles);
    repeat (cycles) @(posedge clk);
  endtask
  task sv_drive(input int v, output int echo);
    data = v;
    echo = v + 1;
  endtask
  function int sv_now(); return $time; endfunction
  always @(data) $display("SV: data=%0d at %0t", data, $time);
  initial begin
    run_seq(3);
    $display("done at %0t", $time);
    $finish;
  end
endmodule

//> C: step 0 at 15 echo 1
//> SV: data=3 at 15
//> C: step 1 at 35 echo 4
//> SV: data=6 at 35
//> C: step 2 at 55 echo 7
//> done at 55
