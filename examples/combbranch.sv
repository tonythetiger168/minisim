// always_comb blocks with branches run as ranked functions; nets and
// variables driven a few bits per assign write their bits directly.
module combbranch;
  logic [3:0] a, b; logic [1:0] op; logic [3:0] y, z; logic f;
  always_comb begin
    z = 0;
    case (op)
      2'd0: y = a + b;
      2'd1: begin y = a - b; z = 1; end
      2'd2: if (a > b) y = a; else y = b;
      default: y = 'x;
    endcase
    f = y[0] ^ z[0];
  end
  logic [3:0] w;
  always_comb if (f) w = y + 1; else w = z;   // reads the other block's outputs
  initial begin
    a = 3; b = 5; op = 0;
    for (int i = 0; i < 12; i++) begin
      #1 $display("%0t op=%0d a=%0d b=%0d y=%0d z=%0d f=%b w=%0d", $time, op, a, b, y, z, f, w);
      op = op + 1; a = a + 3; if (i == 6) b = 'x;
    end
  end

  logic [7:0] pa;
  logic [7:0] rev;
  wire  [7:0] w2;
  wire  [7:0] half;   // only the low nibble driven: stays resolved
  logic [3:0] s;
  for (genvar k = 0; k < 8; k++) begin : g
    assign rev[k] = pa[7-k];
  end
  assign w2[3:0] = pa[3:0] ^ 4'hf;
  assign w2[7:4] = s;
  assign half[3:0] = pa[3:0];
  initial begin
    pa = 8'b1010_0011; s = 4'bx01z;
    #20 $display("%b %b %b", rev, w2, half);
    pa = 8'hx5; s = 4'h9;
    #1 $display("%b %b %b", rev, w2, half);
  end
endmodule
