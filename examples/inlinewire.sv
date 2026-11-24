// Single-use comb wires: msim -compile's cycle domain computes each in its
// one reader (signed reads, a narrower wire that cuts its value, a reader
// with if / case), and still shows their values to $monitor.
module inlinewire;
  logic clk = 0;
  logic [7:0] q = 8'd3, r = 8'd200;
  logic [3:0] narrow;            // cuts q + r to 4 bits
  logic signed [7:0] sw;         // read signed
  logic [8:0] sum9;
  logic [7:0] pick;
  logic flag;
  logic [15:0] acc = 0;
  assign narrow = q + r;
  assign sw = q - r;
  assign sum9 = {1'b0, q} + {1'b0, r};
  assign flag = q[0] ^ r[1];
  always_comb begin
    if (flag) pick = {narrow, narrow};
    else case (q[1:0])
      2'd0: pick = sw >>> 2;
      2'd1: pick = sum9[8:1];
      default: pick = 8'h5a;
    endcase
  end
  always_ff @(posedge clk) begin
    q <= q * 8'd5 + 8'd1;
    r <= r ^ (q << 1);
    acc <= acc * 16'd3 + {8'd0, pick} + (sw < 0 ? 16'd7 : 16'd0);
  end
  initial begin
    repeat (40) begin #1 clk = 1; #1 clk = 0; end
    $display("acc=%0d q=%0d r=%0d", acc, q, r);
    $monitor("mon narrow=%0d sw=%0d", narrow, sw);
    repeat (3) begin #1 clk = 1; #1 clk = 0; end
    $finish;
  end
endmodule
