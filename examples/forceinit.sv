// force / release of a constant from an initial block, the way a simulation
// model holds a signal (a PLL's lock output) until a condition. Nothing else
// writes the signal, so msim -compile runs the force through the kernel and
// the design stays compiled (no mixed mode). Every mode must print the same.
module forceinit_pll (output logic locked, input logic ref_clk, input logic rst);
  initial begin
    force locked = 1'b0;
    wait (rst == 1'b1);
    wait (rst == 1'b0);
    repeat (5) @(posedge ref_clk);
    @(negedge ref_clk);
    force locked = 1'b1;
  end
endmodule

module forceinit;
  logic clk = 0, rst = 0;
  logic locked;
  logic [7:0] a = 0, w, r = 0, q = 0;
  assign w = a + 8'd1;
  always @(posedge clk) r <= r + 1;
  always @(posedge clk) if (locked) q <= q + w;
  forceinit_pll u_pll (.locked(locked), .ref_clk(clk), .rst(rst));
  always #5 clk = ~clk;
  initial begin
    #12 rst = 1;
    #20 rst = 0;
    repeat (8) @(posedge clk);
    #1 $display("locked %b q %0d r %0d w %0d", locked, q, r, w);
    force u_pll.locked = 1'b0;   // from outside, a constant: still compiled
    @(posedge clk); #1 $display("held locked %b q %0d", locked, q);
    release u_pll.locked;
    @(posedge clk); #1 $display("released locked %b q %0d", locked, q);
    $finish;
  end
endmodule
