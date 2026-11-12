// IEEE 1800-2023 10.6.2, 13.3: force and release of a net inside a task.
// When the task releases the net, the continuous assignment that drives it
// takes over again at once, even if nothing it reads has changed since the
// force.
module drv_core(input logic clk, output logic req);
  logic [1:0] st = 0;
  always_ff @(posedge clk) st <= 2'd2;
  assign req = (st == 2'd2);
endmodule
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  wire req;
  drv_core u(.*);
  task automatic block_req(input int cycles);
    force req = 1'b0;
    repeat (cycles) @(posedge clk);
    $display("held %b", req);
    release req;
  endtask
  initial begin
    #12 $display("before %b", req);
    block_req(3);
    #1 $display("after %b", req);
    $finish;
  end
endmodule
//> before 1
//> held 0
//> after 1
