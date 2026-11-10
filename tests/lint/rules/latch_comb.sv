//! args: -only latch,comb -enable LAT002 -top lc
module lc(input logic a, b, c, en, input logic [3:0] d, output logic x, y, z, l, p, output logic [3:0] bits, parts);
  logic t, lp1, lp2, lp3, s1;     //~ COMB002 COMB002
  // L001: bit-accurate.
  always_comb begin
    if (en) x = a;                //~ L001
    for (int i = 0; i < 4; i++) bits[i] = d[i];   // every bit, in a loop
    parts[1:0] = d[1:0];
    if (en) parts[3:2] = d[3:2];  //~ L001
  end
  always @* begin                 //~ LAT001
    if (en) y = a;
  end
  always @* begin
    z = 1'b0;                     // a default first: no latch
    if (en) z = b;
  end
  always @(a or b) begin          //~ COMB001
    p = a & b & c;
  end
  always_latch                    //~ LAT002
    if (en) l = a;

  // A loop through two assigns, and one through a block.
  assign lp1 = lp2 | a;           
  assign lp2 = lp1 & b;
  always_comb lp3 = lp3 ^ c;
  // Not loops: a value made earlier in the same block, and a bit chain.
  always_comb begin
    t = a;
    t = t & b;
    s1 = t;
  end
  logic [4:0] carry;              
  assign carry[0] = a;
  for (genvar i = 0; i < 4; i++) begin : g_c
    assign carry[i+1] = carry[i] & d[i];
  end
  logic unused_ok;
  assign unused_ok = &{carry, s1, lp3, t};
endmodule
