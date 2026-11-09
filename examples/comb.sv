// Combinational logic that msim runs on its static (levelized) schedule:
// a chain of continuous assigns declared out of order, an always_comb that
// reads its own output, and an @* block with a
// $display, which stays an ordinary process.
module comb;
  reg [7:0] a, b;
  reg [7:0] y, z;
  wire [7:0] w3, w2, w1;
  wire [7:0] p;
  assign w3 = w2 + w1;      // declared before its inputs
  assign w2 = w1 ^ 8'h0f;
  assign w1 = a & b;
  assign p = {a[3:0], w3[7:4]};
  always_comb begin
    y = a + b;
    z = y ^ w3;
  end
  always @* if (w1[0]) $display("w1 odd: %b", w1);
  initial begin
    a = 8'd1; b = 8'd3;
    #1 $display("w1=%d w2=%d w3=%d p=%b y=%d z=%d", w1, w2, w3, p, y, z);
    a = 8'd6;
    #1 $display("w1=%d w2=%d w3=%d p=%b y=%d z=%d", w1, w2, w3, p, y, z);
    b = 8'd255;
    #1 $display("w1=%d w2=%d w3=%d p=%b y=%d z=%d", w1, w2, w3, p, y, z);
  end
endmodule
