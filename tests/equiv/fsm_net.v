// fsm.sv synthesized by Yosys (synth -flatten; abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX).

module fsm(clk, rst_n, go, din, dout, busy, done);
  wire [3:0] _00_;
  wire _01_;
  wire [2:0] _02_;
  wire _03_;
  wire _04_;
  wire _05_;
  wire _06_;
  wire _07_;
  wire _08_;
  wire _09_;
  wire _10_;
  wire _11_;
  wire _12_;
  reg [3:0] acc;
  output busy;
  reg busy;
  input clk;
  wire clk;
  input [3:0] din;
  wire [3:0] din;
  output done;
  wire done;
  output [3:0] dout;
  reg [3:0] dout;
  input go;
  wire go;
  reg [2:0] n;
  wire [1:0] nx;
  input rst_n;
  wire rst_n;
  reg [1:0] st;
  assign _03_ = ~st[0];
  assign _04_ = _03_ & st[1];
  assign _05_ = n[1] | n[0];
  assign _06_ = ~(n[2] | _05_);
  assign _07_ = ~(_04_ & _06_);
  assign _08_ = ~(go & _03_);
  assign _09_ = st[1] | _08_;
  assign nx[0] = ~(_07_ & _09_);
  assign nx[1] = st[0] ^ st[1];
  assign _01_ = nx[0] | nx[1];
  assign done = st[0] & st[1];
  assign _02_[0] = ~(n[0] & _04_);
  assign _10_ = ~(n[1] ^ n[0]);
  assign _02_[1] = _04_ & _10_;
  assign _11_ = n[2] ^ _05_;
  assign _02_[2] = ~(_04_ & _11_);
  assign _12_ = acc[3] ^ acc[0];
  assign _00_[0] = _04_ ? _12_ : din[0];
  assign _00_[1] = _04_ ? acc[0] : din[1];
  assign _00_[2] = _04_ ? acc[1] : din[2];
  assign _00_[3] = _04_ ? acc[2] : din[3];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) n[0] <= 1'h0;
    else if (nx[1]) n[0] <= _02_[0];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) n[1] <= 1'h0;
    else if (nx[1]) n[1] <= _02_[1];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) n[2] <= 1'h0;
    else if (nx[1]) n[2] <= _02_[2];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) busy <= 1'h0;
    else busy <= _01_;
  always @(posedge clk, negedge rst_n)
    if (!rst_n) st[0] <= 1'h0;
    else st[0] <= nx[0];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) st[1] <= 1'h0;
    else st[1] <= nx[1];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) dout[0] <= 1'h0;
    else if (done) dout[0] <= acc[0];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) dout[1] <= 1'h0;
    else if (done) dout[1] <= acc[1];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) dout[2] <= 1'h0;
    else if (done) dout[2] <= acc[2];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) dout[3] <= 1'h0;
    else if (done) dout[3] <= acc[3];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) acc[0] <= 1'h0;
    else if (nx[1]) acc[0] <= _00_[0];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) acc[1] <= 1'h0;
    else if (nx[1]) acc[1] <= _00_[1];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) acc[2] <= 1'h0;
    else if (nx[1]) acc[2] <= _00_[2];
  always @(posedge clk, negedge rst_n)
    if (!rst_n) acc[3] <= 1'h0;
    else if (nx[1]) acc[3] <= _00_[3];
endmodule
