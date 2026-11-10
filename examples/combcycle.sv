// Combinational blocks that feed each other as whole blocks (no bit of one
// depends on itself), and a unique case whose violation is reported once per
// change, not once per evaluation. All of them run as ranked functions.
module combcycle;
  logic clk = 0;
  logic [3:0] a, b;
  logic [3:0] p, q, r, t;
  logic [2:0] s; logic [3:0] y, acc;
  always #5 clk = ~clk;
  // Block X writes p (from a) and r (from q); block Y writes q (from p) and t (from r).
  always_comb begin
    p = a + 4'd1;
    if (q[0]) r = q ^ 4'h5; else r = q + b;
  end
  always_comb begin
    q = p ^ b;
    case (r[1:0])
      2'd0: t = r;
      2'd1: t = ~r;
      default: t = r + p;
    endcase
  end
  always_comb begin
    y = 4'd0;
    unique case (1'b1)
      s[0]: y = 4'd1;
      s[1]: y = 4'd2;
      s[2]: y = t;
    endcase
  end
  always_ff @(posedge clk) acc <= acc + y + t;
  initial begin
    acc = 0; a = 0; b = 3; s = 3'b001;
    for (int i = 0; i < 10; i++) begin
      #10 $display("%0t a=%0d b=%0d p=%0d q=%0d r=%0d t=%0d y=%0d acc=%0d", $time, a, b, p, q, r, t, y, acc);
      a = a + 3; b = b ^ a;
      s = (i == 3) ? 3'b011 : (i == 6) ? 3'b000 : 3'b001 << (i % 3);   // i = 3: two match; i = 6: none
    end
    s = 3'b110; s = 3'b100;   // a glitch: no report
    #10 $display("%0t y=%0d acc=%0d", $time, y, acc);
    $finish;
  end
endmodule
