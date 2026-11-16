// msim -compile stores a register at its NBA even when its own process
// reads it afterwards: those reads take a copy made as the process starts
// (the value before this edge). Defaults written first and read later, a
// register read after several writes, and one read by the comb logic; 40
// units so the parallel cycle domain runs too.
module snapunit(input clk, input rst, input [7:0] din, output [15:0] sum);
  reg [7:0] st, cnt, acc;
  reg [15:0] tot;
  reg busy;
  always @(posedge clk) begin
    busy <= 1'b0;                 // default, read below
    st <= st + 8'd1;
    if (busy) acc <= acc ^ st;    // old busy, old st
    else acc <= acc + din;
    if (cnt[0]) tot <= tot + {8'd0, st};       // old cnt, old st, old tot
    if (cnt[1]) tot <= tot + {8'd0, st} + 1;
    if (cnt[2]) tot <= tot + {8'd0, acc} + 2;
    cnt <= cnt + st[1:0] + 1;
    if (st[2] && !busy) busy <= 1'b1;
    if (rst) begin st <= 0; cnt <= 0; acc <= 0; tot <= 0; busy <= 0; end
  end
  assign sum = tot ^ {acc, cnt};
endmodule
module snapquad(input clk, input rst, input [7:0] base, output [15:0] q);
  wire [15:0] a, b, c, d;
  snapunit u0(.clk(clk), .rst(rst), .din(base), .sum(a));
  snapunit u1(.clk(clk), .rst(rst), .din(base + 8'd1), .sum(b));
  snapunit u2(.clk(clk), .rst(rst), .din(base + 8'd2), .sum(c));
  snapunit u3(.clk(clk), .rst(rst), .din(base + 8'd3), .sum(d));
  assign q = a ^ {b[14:0], b[15]} ^ {c[13:0], c[15:14]} ^ {d[12:0], d[15:13]};
endmodule
module snapnba;
  reg clk = 0, rst = 1;
  wire [15:0] q0, q1, q2, q3, q4, q5, q6, q7, q8, q9;
  snapquad a0(clk, rst, 8'd10, q0);
  snapquad a1(clk, rst, 8'd20, q1);
  snapquad a2(clk, rst, 8'd30, q2);
  snapquad a3(clk, rst, 8'd40, q3);
  snapquad a4(clk, rst, 8'd50, q4);
  snapquad a5(clk, rst, 8'd60, q5);
  snapquad a6(clk, rst, 8'd70, q6);
  snapquad a7(clk, rst, 8'd80, q7);
  snapquad a8(clk, rst, 8'd90, q8);
  snapquad a9(clk, rst, 8'd100, q9);
  always #1 clk = ~clk;
  integer r;
  initial begin
    #5 rst = 0;
    for (r = 0; r < 4; r = r + 1) begin
      #200;
      $display("round %0d: %h %h %h %h %h %h %h %h %h %h", r, q0, q1, q2, q3, q4, q5, q6, q7, q8, q9);
    end
    $finish;
  end
endmodule
