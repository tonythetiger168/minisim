// Without -xprop the same design keeps the standard (optimistic) behavior.
module top;
  logic c, a, b; logic [3:0] y, z, p, q; logic [1:0] sel; logic [3:0] w;
  logic clk, en, rst_n; logic [3:0] d, r, r2, r3;
  always_comb begin
    if (c) y = 4'b1010; else y = 4'b1001;
  end
  always_comb begin
    z = 4'b0000;
    if (c) z = 4'b0011;
  end
  always_comb begin
    case (sel)
      2'b00: w = 4'h1;
      2'b01: w = 4'h3;
      2'b10: w = 4'h4;
      default: w = 4'h8;
    endcase
  end
  always @(posedge clk) if (en) r <= d;
  always @(posedge clk or negedge rst_n)
    if (!rst_n) r2 <= 0; else begin r2 <= 4'h5; if (en) r2 <= d; end
  always_ff @(posedge clk) begin
    r3[1:0] <= d[1:0];
    if (en) r3[3:2] <= d[3:2];
  end
  initial begin
    c = 1'bx; sel = 2'b0x; #1 $display("y=%b z=%b w=%b", y, z, w);
    sel = 2'bxx; #1 $display("w=%b", w);
    sel = 2'bx1; #1 $display("w=%b", w);
    c = 1; sel = 2; #1 $display("y=%b z=%b w=%b", y, z, w);
    c = 0; sel = 1; #1 $display("y=%b z=%b w=%b", y, z, w);
    // flops
    clk = 0; rst_n = 1; en = 1; d = 4'b1100; #1 clk = 1; #1 $display("r=%b r2=%b r3=%b", r, r2, r3);
    clk = 0; en = 1'bx; d = 4'b1010; #1 clk = 1; #1 $display("r=%b r2=%b r3=%b (en x)", r, r2, r3);
    clk = 0; en = 1; d = 4'b0110; #1 clk = 1; #1 $display("r=%b r2=%b r3=%b", r, r2, r3);
    clk = 0; d = 4'b0101; #1 clk = 1'bx; #1 $display("r=%b r2=%b r3=%b (clk 0->x)", r, r2, r3);
    clk = 1; #1 $display("r=%b r2=%b r3=%b (clk x->1)", r, r2, r3);
    clk = 0; #1 clk = 1; #1 $display("r=%b r2=%b r3=%b", r, r2, r3);
    rst_n = 1'bx; #1 $display("r=%b r2=%b (rst 1->x)", r, r2);
    rst_n = 0; #1 $display("r=%b r2=%b (rst x->0)", r, r2);
    $finish;
  end
endmodule
//> y=1001 z=0000 w=1000
//> w=1000
//> w=1000
//> y=1010 z=0011 w=0100
//> y=1001 z=0000 w=0011
//> r=1100 r2=1100 r3=1100
//> r=1100 r2=0101 r3=1110 (en x)
//> r=0110 r2=0110 r3=0110
//> r=0101 r2=0101 r3=0101 (clk 0->x)
//> r=0101 r2=0101 r3=0101 (clk x->1)
//> r=0101 r2=0101 r3=0101
//> r=0101 r2=0101 (rst 1->x)
//> r=0101 r2=0000 (rst x->0)
