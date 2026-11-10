// A reference design for mequiv; mem_net.v is its synthesized netlist.
module mem(input clk, rst, input we, input [2:0] wa, ra, input [7:0] wd, input g, input [7:0] li, output [7:0] rd, output reg [7:0] lo, output reg [7:0] cnt, output [15:0] prod, output [7:0] bs, output ov);
  reg [7:0] mem [0:7];
  integer i;
  always @(posedge clk) begin
    if (rst) for (i=0;i<8;i=i+1) mem[i] <= i[7:0];
    else if (we) mem[wa] <= wd;
  end
  assign rd = mem[ra];
  always @* if (g) lo = li;
  always @(posedge clk or posedge rst) if (rst) cnt <= 8'hff; else if (we) cnt <= cnt + wd; 
  assign prod = $signed(wd) * $signed(li);
  assign bs = wd[ra +: 2] + li[7 -: 4];
  assign ov = &cnt[3:0] | (wd == 8'd3) | (wd == 8'd9);
endmodule
