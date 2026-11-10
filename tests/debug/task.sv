// mdebug self-test: tasks, call outputs and force as drivers.
module tk;
  logic clk = 0;
  logic [7:0] a = 0, q, w, f;
  wire  [7:0] n = a + 8'd1;
  task automatic bump(input logic [7:0] x, output logic [7:0] y);
    y = x + 8'd2;
  endtask
  task set_q(input logic [7:0] v);
    if (v != 0) q = v;
  endtask
  always #5 clk = ~clk;
  always @(posedge clk) bump(a, w);
  always_comb f = n;
  initial begin
    #12 a = 8'd3;
    #10 set_q(8'd7);
    #10 force f = 8'haa;
    #10 release f;
    #10 $display("w=%0d q=%0d f=%0d", w, q, f);
    $finish;
  end
endmodule
