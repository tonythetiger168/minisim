// Port copies and logic nobody reads. msim -compile reads a copied signal at
// its source and skips statements whose results have no reader; a waveform
// (simv -vcd) or a late hierarchical read must still see every value.
module dc_leaf(input logic clk, input logic [7:0] a, output logic [7:0] y,
               output logic [7:0] spare_o, output logic [3:0] cnt_o);
  logic [7:0] t, r;
  logic [3:0] seen_q;
  assign t = a;                       // a copy of a copy of the testbench value
  always_ff @(posedge clk) r <= t + 8'd3;
  assign y = r;
  always_comb begin                   // only the waveform reads spare_o
    spare_o = 8'h00;
    if (t[0]) spare_o = t ^ 8'h5a;
  end
  always_ff @(posedge clk) if (!$isunknown(t)) seen_q <= seen_q + {3'b0, t[1]};
  assign cnt_o = seen_q;              // read once, by name, at the end
  initial seen_q = 0;
endmodule

module dc_mid(input logic clk, input logic [7:0] a, output logic [7:0] y);
  logic [7:0] a_i, y_i, spare;
  logic [3:0] cnt;
  logic [7:0] dbg;
  assign a_i = a;
  dc_leaf l(.clk(clk), .a(a_i), .y(y_i), .spare_o(spare), .cnt_o(cnt));
  assign dbg = spare + {4'b0, cnt};   // dead, and fed by dead logic
  assign y = y_i;
endmodule

module deadcopy;
  logic clk = 0;
  logic [7:0] d = 0, q;
  logic [31:0] sum = 0;
  always #5 clk = ~clk;
  logic [7:0] d_r;
  always_ff @(posedge clk) d_r <= d;
  dc_mid m(.clk(clk), .a(d_r), .y(q));
  always_ff @(posedge clk) begin
    d <= d * 8'd5 + 8'd1;
    if (!$isunknown(q)) sum <= sum * 31 + q;
  end
  initial begin
    #400;
    $display("sum=%h q=%h cnt=%0d", sum, q, m.cnt);
    $finish;
  end
endmodule
