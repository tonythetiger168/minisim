// NBAs to parts of a register private to the cycle domain: the commit
// merges each part into the register, in the order the NBAs ran.
module pp(input logic clk, input logic rst, input logic [3:0] k, output logic [15:0] o);
  logic [15:0] r;
  logic [7:0] c;
  always_ff @(posedge clk) begin
    if (rst) r <= 16'h0;
    else begin
      r[3:0] <= r[3:0] + k;
      if (c[0]) r[15:8] <= r[7:0] ^ c;
      r[7:4] <= r[11:8];
      if (c[1]) r <= {r[7:0], r[15:8]};
      if (c[2]) r[0] <= ~r[15];
    end
    c <= rst ? 8'h0 : c + 8'd3;
  end
  assign o = r ^ {c, c};
endmodule
module privpart;
  logic clk = 0, rst = 1;
  logic [15:0] o0, o1;
  pp a(.clk(clk), .rst(rst), .k(4'd5), .o(o0));
  pp b(.clk(clk), .rst(rst), .k(4'd9), .o(o1));
  always #5 clk = ~clk;
  int n = 0;
  always @(posedge clk) if (!rst) begin
    n <= n + 1;
    if (n % 7 == 0) $display("%0t %h %h", $time, o0, o1);
  end
  initial begin
    #12 rst = 0;
    #800 $finish;
  end
endmodule
