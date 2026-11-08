// IEEE 1800-2023 25.3, 25.5: an interface with a port, modports with
// input/output directions, interface ports with a modport (bus_if.src b),
// named connection, and a task declared in the interface called as bus.send().
//! icarus-skip: Icarus 13 does not support interface ports
interface bus_if(input logic clk);
  logic [7:0] data;
  logic valid;
  modport src(output data, output valid, input clk);
  modport dst(input data, input valid, input clk);
  task automatic send(input logic [7:0] d);
    @(posedge clk);
    data <= d;
    valid <= 1;
    @(posedge clk);
    valid <= 0;
  endtask
endinterface

module producer(bus_if.src b);
  initial begin
    b.valid = 0;
    repeat (2) @(posedge b.clk);
    b.data <= 8'h11; b.valid <= 1;
    @(posedge b.clk) b.valid <= 0;
  end
endmodule

module consumer(bus_if.dst b);
  always @(posedge b.clk) if (b.valid) $display("%0t got %h", $time, b.data);
endmodule

module top;
  logic clk = 0;
  always #5 clk = ~clk;
  bus_if bus(clk);
  producer p(bus);
  consumer c(.b(bus));
  initial begin
    #60 bus.send(8'h22);
    #40 $finish;
  end
endmodule

//> 25 got 11
//> 75 got 22
