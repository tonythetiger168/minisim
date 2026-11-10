//! args: -only clock,reset -enable CLK002,RST004 -top cr
module cr(input logic clk, clk2, rst_n, arst, input logic d, e, output logic [7:0] q, output logic g);  //~ CLK003
  logic [7:0] init;
  assign init = {8{d}};
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) q[0] <= 1'b0;
    else        q[0] <= d;
  always_ff @(posedge clk or negedge rst_n) begin   //~ RST003
    if (!rst_n) q[1] <= 1'b0;
    else begin
      q[1] <= d;
      g <= e;                     // no reset for g in a reset block
    end
  end
  always_ff @(posedge clk or posedge rst_n)         //~ RST005
    if (!rst_n) q[2] <= 1'b0;
    else        q[2] <= d;
  always_ff @(posedge clk or posedge rst_n)         //~ RST001
    if (rst_n) q[3] <= 1'b0;
    else       q[3] <= d;
  always_ff @(posedge clk or posedge arst)
    if (arst) q[4] <= init[4];    //~ RST006
    else      q[4] <= d;
  always_ff @(posedge clk or posedge arst)          //~ RST007
    q[5] <= arst ? 1'b0 : d;
  always_ff @(posedge clk)                          //~ RST002
    if (arst) q[6] <= 1'b0;       // arst is asynchronous above
    else      q[6] <= d;
  always_ff @(negedge clk)                          //~ CLK002 RST004
    q[7] <= d & clk2;             //~ CLK001
  logic x, yy;
  always_ff @(posedge clk2) x <= d;                 //~ RST004
  always_comb yy = x & clk;       //~ CLK001
  logic gclk;
  assign gclk = clk;              // a plain buffer is not data
  logic z;
  always_ff @(posedge gclk or posedge clk2)
    if (clk2) z <= 1'b0;
    else      z <= yy;
  logic unused_z;
  assign unused_z = z;
endmodule
