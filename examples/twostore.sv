// msim -compile: the 2-state copy of a clocked write stores the value word
// only (the x and z words are already 0 whenever 2-state mode runs). The
// registers here start unknown, so the run begins in 4-state mode and
// switches once reset has cleared them; a register that is never reset
// keeps the domain in 4-state mode until it is assigned.
module twostore;
  logic clk = 0, rst = 0;
  logic [7:0] a, b, late;     // no initializer: x until reset / the first write
  logic [15:0] acc;
  logic       go;
  logic [7:0] q1, q2;         // a pipeline nobody else reads: stored directly
  // The testbench reads wires, not the registers: a register an initial
  // block reads is committed through the list instead of stored directly.
  wire [7:0] a_o = a, b_o = b, late_o = late, q2_o = q2;
  wire [15:0] acc_o = acc;
  always #5 clk = ~clk;
  always_ff @(posedge clk) begin
    if (rst) begin a <= 8'd1; b <= 8'd2; acc <= 16'd0; end
    else begin
      a <= a + b;
      b <= b ^ {a[6:0], a[7]};
      acc <= acc + {8'd0, a};
    end
  end
  always_ff @(posedge clk) if (go) late <= a ^ b;
  always_ff @(posedge clk) begin q1 <= a + 8'd3; q2 <= q1; end
  initial begin
    go = 0;
    #12 $display("before reset: a=%b acc=%b q2=%b", a_o, acc_o, q2_o);
    rst = 1;
    #20 rst = 0;
    #30 $display("running: a=%0d b=%0d acc=%0d late=%b q2=%0d", a_o, b_o, acc_o, late_o, q2_o);
    go = 1;
    #40 $display("all known: a=%0d b=%0d acc=%0d late=%0d q2=%0d", a_o, b_o, acc_o, late_o, q2_o);
    rst = 1;
    #10 rst = 0;
    #50 $display("after second reset: a=%0d b=%0d acc=%0d late=%0d q2=%0d", a_o, b_o, acc_o, late_o, q2_o);
    $finish;
  end
endmodule
