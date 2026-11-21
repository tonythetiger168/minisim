// msim -compile: a wide (> 64 bit) write or NBA that leaves the value as
// it was is found by comparing the words in place (sameAsMasked), without
// a masked copy. The waiters on the vector must still see every change,
// including x / z bits and bits only in the top limb. A wide concatenation
// copies each part into place (CAT, v_concat).
module widesame;
  logic clk = 0;
  logic [159:0] q, d;
  logic [99:0] c;
  logic [101:0] k;   // a wide concatenation of five parts, x and z included
  int qn = 0, cn = 0;
  always #5 clk = ~clk;
  always_ff @(posedge clk) q <= d;
  always @* c = {q[159:100], q[39:0]};
  always @* k = {q[39:0], 4'bx1z0, c[99:60], q[159:150], 8'hA5};
  always @(q) qn++;
  always @(c) cn++;
  initial begin
    d = '0;
    for (int i = 0; i < 30; i++) begin
      @(negedge clk);
      if (i % 3 == 0) d[159] = ~d[159];          // top limb only
      if (i % 5 == 0) d[70 +: 8] = 8'(i);        // a middle limb, outside c
      if (i == 7) d[3:0] = 4'bx1z0;
      if (i == 12) d[3:0] = 4'b0000;
    end
    @(negedge clk);
    $display("q=%h c=%h k=%b qn=%0d cn=%0d", q, c, k, qn, cn);
    $finish;
  end
endmodule
