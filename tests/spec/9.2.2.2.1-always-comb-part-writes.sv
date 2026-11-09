// IEEE 1800-2023 9.2.2.2.1 always_comb sensitivity: an expression the block
// also writes is not in its sensitivity list, so blocks that each build one
// bit of a shared vector settle instead of waking each other, even while
// their inputs are x. A bit the block reads but does not write still wakes it.
//! icarus-skip: Icarus 13 makes the block sensitive to all of v or none of it, so it misses the change of v[2]
module t;
  logic [1:0] own, sel, d;
  logic [3:0] v;
  logic y;
  for (genvar i = 0; i < 2; i++) begin : g
    always_comb begin
      own[i] = 1'b1;
      for (int j = 0; j < 2; j++)
        if (i == j) own[i] &= sel[j];
        else own[i] &= d[j];
    end
  end
  always_comb begin
    v[1:0] = sel;
    y = v[2];
  end
  assign v[3:2] = d;
  initial begin
    #1 $display("%b %b", own, y);
    sel = 2'b11; d = 2'b10;
    #1 $display("%b %b", own, y);
    d = 2'b11;
    #1 $display("%b %b", own, y);
  end
endmodule
//> xx x
//> 01 0
//> 11 1
