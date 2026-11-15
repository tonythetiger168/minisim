// A clocked block whose working variable is written only in parts (struct
// fields, part selects) and never whole: msim.cpp localizeSeqBlocking makes
// those writes silent stores (Sim::poke), which must merge each part into
// the signal's full width. Every mode prints the same.
module tmppart;
  typedef struct packed { logic [7:0] data; logic ack; logic err; } rsp_t;
  rsp_t dev [0:3];
  rsp_t host;
  logic [9:0] t, h2;
  logic clk = 0;
  always #1 clk = ~clk;
  always @(posedge clk) begin
    static rsp_t v;
    v.data = 8'h00;
    v.ack = 1'b0;
    v.err = 1'b0;
    for (int i = 0; i < 4; i++)
      if (i != 1) begin
        v.data = v.data | dev[i].data;
        v.ack = v.ack | dev[i].ack;
        v.err = v.err | dev[i].err;
      end
    host <= v;
  end
  always @(posedge clk) begin
    t[9:2] = dev[0].data;
    t[1] = dev[2].ack;
    h2 <= t;
  end
  always @(host or h2) $display("%0t host=%h h2=%b", $time, host, h2);
  initial begin
    for (int k = 0; k < 4; k++) dev[k] = '0;
    #3 dev[0] = {8'h01, 1'b1, 1'b0};
    #4 dev[0] = '0; dev[2] = {8'h40, 1'b1, 1'b0};
    #4 dev[2] = '0;
    #4 dev[3] = {8'h80, 1'b0, 1'b1};
    #4 $finish;
  end
endmodule
