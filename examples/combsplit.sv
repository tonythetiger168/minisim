// A multi-output always_comb whose outputs feed each other through other
// blocks. Taken as one block it looks like a combinational loop; split by
// output it is a plain chain.
module combsplit;
  logic clk = 0;
  logic [7:0] cnt = 0;
  logic [1:0] sel;
  logic [7:0] a, b, c, d, e;
  logic       v, w;
  typedef enum logic [1:0] {S0, S1, S2, S3} st_t;
  st_t st;

  assign sel = cnt[1:0];
  assign st  = st_t'(cnt[3:2]);
  // b depends on a (other block), c depends on b: a -> b -> c through
  // the same block twice.
  assign b = a + 8'd3;
  assign d = c ^ 8'h5a;

  always_comb begin
    a = 8'd0;
    c = 8'd0;
    v = 1'b0;
    w = 1'b0;
    unique case (st)
      S0: begin a = cnt;        c = b;         v = 1'b1; end
      S1: begin a = cnt << 1;   c = b + 8'd1;  end
      S2: begin a = ~cnt;       c = b & 8'h0f; w = v;    end
      default: begin
        a = cnt ^ 8'haa;
        if (sel == 2'd1) c = b | 8'h80;
        else             c = b;
        w = 1'b1;
      end
    endcase
  end

  // an output read inside the block that assigns it stays in one piece
  always_comb begin
    e = d;
    if (v) e = e + 8'd1;
    if (w) e = {e[3:0], e[7:4]};
  end

  logic [31:0] sum = 0;
  always_ff @(posedge clk) begin
    cnt <= cnt + 8'd1;
    sum <= sum * 32'd31 + {a, c, d, e};
  end

  always #5 clk = ~clk;
  initial begin
    #2000;
    $display("sum=%h a=%h c=%h d=%h e=%h v=%b w=%b", sum, a, c, d, e, v, w);
    $finish;
  end
endmodule
