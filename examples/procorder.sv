// msim -compile: the clocked processes of the cycle domain run readers
// first (codegen.hpp orderDomain), so a register another process reads can
// be stored at once instead of through the commit list. Each reader must
// still see the value from before the clock edge (9.4.2, 10.4.2): here the
// processes read each other's registers in a cycle (a -> b -> c -> a),
// write parts of them, reset asynchronously, and a process on a second
// clock reads a register of the first clock's processes.
module ordunit #(parameter [7:0] K = 8'h11) (input clk, input clk2, input rst_n, output [31:0] sig);
  reg [7:0] a, b = 8'h3, c, d = 0;
  reg [15:0] e;
  reg [7:0] f = 0;
  always @(posedge clk or negedge rst_n)        // writes a, reads c
    if (!rst_n) a <= K;
    else a <= a + c + 8'd1;
  reg [7:0] g = 1, h = 0;
  always @(posedge clk) begin                   // writes b and g, reads a, e and d
    b <= {a[3:0], e[7:4]} ^ b;
    g <= d[1] ? g + 8'd5 : g + 8'd3;
  end
  always @(posedge clk or negedge rst_n)        // writes c and part of e, reads b and d
    if (!rst_n) begin c <= 0; e <= 16'h0; end
    else begin
      c <= b - d;
      e[7:0] <= c ^ b;
      if (b[0]) e[15:8] <= a;
    end
  always @(posedge clk) begin                   // writes d and h, reads a, b, c and g
    d <= (a & b) | c;
    h <= h ^ g;
  end
  always @(posedge clk2)                        // another clock: reads a and e
    f <= a ^ e[15:8] ^ f;
  assign sig = {a ^ b ^ g, c ^ d, e ^ {h, f}};
endmodule
module procorder;
  reg clk = 0, clk2 = 0, rst_n = 1;
  wire [31:0] s0, s1;
  ordunit #(.K(8'h11)) u0(.clk(clk), .clk2(clk2), .rst_n(rst_n), .sig(s0));
  ordunit #(.K(8'h5a)) u1(.clk(clk), .clk2(clk2), .rst_n(rst_n), .sig(s1));
  always #2 clk = ~clk;
  always #3 clk2 = ~clk2;
  integer k;
  initial begin
    #1 rst_n = 0;
    #4 rst_n = 1;
    for (k = 0; k < 8; k = k + 1) begin
      #13 $display("t=%0t s=%h %h", $time, s0, s1);
      if (k == 4) begin rst_n = 0; #1 rst_n = 1; end
    end
    $finish;
  end
endmodule
