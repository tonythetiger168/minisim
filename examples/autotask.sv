// Automatic tasks that only one procedure calls (directly or through
// another task) cannot overlap, so they lower without frames and
// msim -compile runs them (13.3.2). Their locals still start fresh on
// every call.
module autotask;
  logic clk = 0;
  logic [7:0] sh = 0;
  task automatic tick(input logic d);
    sh = {sh[6:0], d};
    #1 clk = 1;
    #1 clk = 0;
  endtask
  task automatic send(input logic [7:0] v, output int n);
    int k = 0;            // initialized on every call
    logic [3:0] u;        // x on every call
    if (u !== 4'bx) $display("u kept %b", u);
    u = 4'h5;
    for (int i = 7; i >= 0; i--) begin
      tick(v[i]);
      k = k + 1;
    end
    n = k;
  endtask
  int cnt = 0;
  always @(posedge clk) cnt <= cnt + 1;
  initial begin
    int n;
    send(8'hA5, n);
    $display("sh=%h n=%0d t=%0t", sh, n, $time);
    send(8'h3C, n);
    $display("sh=%h n=%0d t=%0t cnt=%0d", sh, n, $time, cnt);
    $finish;
  end
endmodule
