// IEEE 1800-2023 10.6.2: force and release of a constant bit or part select
// of a net, and of a concatenation of a variable, a net and a part. The other
// bits keep following their drivers; release lets them resolve again, and a
// released variable keeps its value.
//! icarus-skip: Icarus 13 cannot force a bit select
module t;
  wire [7:0] w;
  wire n;
  logic [7:0] d = 8'h00;
  logic r, b = 0, e = 0;
  assign w = d;
  assign n = e;
  initial begin
    force w[7] = b;
    #1 $display("%h", w);
    d = 8'h3C; b = 1;
    #1 $display("%h", w);
    release w[7];
    #1 $display("%h", w);
    force w[1] = 1'b0; force w[6:5] = 2'b10;
    #1 $display("%h", w);
    d = 8'hFF;
    #1 $display("%h", w);
    release w;
    #1 $display("%h", w);
    force {r, n, w[3:0]} = 6'b10_0110;
    #1 $display("%b %b %h", r, n, w);
    release {r, n, w[3:0]};
    #1 $display("%b %b %h", r, n, w);
  end
endmodule
//> 00
//> bc
//> 3c
//> 5c
//> dd
//> ff
//> 1 0 f6
//> 1 0 ff
