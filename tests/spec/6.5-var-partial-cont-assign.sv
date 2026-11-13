// IEEE 1800-2023 6.5 Nets and variables: a variable may be written by a
// continuous assignment to some of its bits (or members) and by procedures
// to the others. The procedural bits keep their values; they do not become z.
//! icarus-skip: Icarus rejects a variable written by both a continuous assignment and procedures
module drv(input logic clk, output logic a, output logic [3:0] d);
  always @(posedge clk) begin a <= 1'b1; d <= 4'b0101; end
endmodule
module t;
  typedef struct packed { logic ack; logic err; logic [3:0] d; } rsp_t;
  logic clk = 0, en = 0;
  rsp_t rsp [0:2];
  rsp_t one;
  logic [7:0] v;
  drv u (.clk(clk), .a(rsp[1].ack), .d(rsp[1].d));
  assign one.ack = en;
  assign v[3:0] = {4{en}};
  always begin rsp[1].err <= 1'b0; one.err <= 1'b0; v[7:4] = 4'ha; @(clk); end
  initial begin
    #1 clk = 1; en = 1;
    #1 $display("%b %b %b", rsp[1], one, v);
    en = 0;
    #1 $display("%b %b", one, v);
  end
endmodule
//> 100101 10xxxx 10101111
//> 00xxxx 10100000
