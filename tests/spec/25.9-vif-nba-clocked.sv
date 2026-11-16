// IEEE 1800-2023 25.9, 10.4.2: a clocked process that writes an interface
// member through a virtual interface with a nonblocking assignment; the
// write lands in the instance the handle names when the NBA region runs.
//! icarus-skip: Icarus does not support virtual interfaces
interface bus_if;
  logic [15:0] data;
endinterface
module t;
  logic clk = 0;
  bus_if a ();
  bus_if b ();
  virtual bus_if v = a;
  logic [15:0] d = 16'h1234;
  always @(negedge clk) v.data <= d;
  always @(a.data) $display("[%0t] a %h", $time, a.data);
  always @(b.data) $display("[%0t] b %h", $time, b.data);
  initial begin
    #1 clk = 1; #1 clk = 0;
    #1 v = b; d = 16'h5678;
    #1 clk = 1; #1 clk = 0;
    #1 $finish;
  end
endmodule
//> [2] a 1234
//> [5] b 5678
