// IEEE 1800-2023 28.3.6: an array of three-state gates or MOS switches takes
// one bit of each terminal that is as wide as the array; a 1-bit terminal
// goes to every instance.
module t;
  wire [3:0] y, z;
  reg [3:0] a, en;
  reg on;
  bufif1 g[3:0] (y, a, en);
  nmos m[0:3] (z, a, on);
  initial begin
    a = 4'b1010;
    en = 4'b0110;
    on = 1;
    #1 $display("%b %b", y, z);
    on = 0;
    #1 $display("%b", z);
  end
endmodule
//> z01z 1010
//> zzzz
