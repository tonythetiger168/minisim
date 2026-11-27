// force / release and assign / deassign of signals that other processes
// write (a continuous assignment's net, a clocked register) and of an
// expression that follows its inputs: msim -compile runs the forcing block in
// mixed mode. Every mode must print the same.
module forcemix_pll (output logic locked, input logic ref_clk, input logic rst);
  initial begin
    force locked = 1'b0;
    wait (rst == 1'b1);
    wait (rst == 1'b0);
    repeat (5) @(posedge ref_clk);
    @(negedge ref_clk);
    force locked = 1'b1;
  end
endmodule

module forcemix;
  logic clk = 0, rst = 0;
  logic locked;
  logic [7:0] a = 0, w, r = 0, q = 0;
  assign w = a + 8'd1;
  always @(posedge clk) r <= r + 1;
  always @(posedge clk) if (locked) q <= q + w;
  forcemix_pll u_pll (.locked(locked), .ref_clk(clk), .rst(rst));
  always #5 clk = ~clk;
  initial begin
    #12 rst = 1;
    #20 rst = 0;
    repeat (8) @(posedge clk);
    #1 $display("locked %b q %0d r %0d w %0d", locked, q, r, w);
    force w = 8'd100;          // a continuous assignment's net
    force r = a + 8'd7;        // a register: held to an expression that follows a
    a = 3;
    #1 $display("w %0d r %0d", w, r);
    a = 4;
    @(posedge clk); #1 $display("w %0d r %0d q %0d", w, r, q);
    release w;
    release r;
    #1 $display("released w %0d r %0d", w, r);
    @(posedge clk); #1 $display("w %0d r %0d q %0d", w, r, q);
    assign r = 8'd55;
    @(posedge clk); #1 $display("assigned r %0d", r);
    deassign r;
    @(posedge clk); #1 $display("deassigned r %0d q %0d", r, q);
    $finish;
  end
endmodule
