// Run-time bit selects of wide vectors in a function called from a
// continuous assign (a SHA-3 state reshaped lane by lane, as OpenTitan's
// KMAC does): -compile reads and writes the bits in place (codegen.hpp fx
// DynSlice, Sim::setBitsV). Also partial X, an out-of-range index and a
// multi-bit select crossing a 64-bit limb.
module widebits;
  localparam int W = 16;
  typedef logic [4:0][4:0][W-1:0] box_t;
  function automatic box_t to_box(logic [25*W-1:0] in);
    box_t b;
    for (int y = 0; y < 5; y++)
      for (int x = 0; x < 5; x++)
        for (int z = 0; z < W; z++)
          b[x][y][z] = in[W*(5*y + x) + z];
    return b;
  endfunction
  function automatic logic [25*W-1:0] from_box(box_t b);
    logic [25*W-1:0] o;
    for (int y = 0; y < 5; y++)
      for (int x = 0; x < 5; x++)
        o[W*(5*y + x) +: W] = b[x][y];
    return o;
  endfunction
  logic [25*W-1:0] st, back;
  box_t bx;
  logic [7:0] xlimb;
  logic [3:0] oor;
  int idx;
  assign bx = to_box(st);
  assign back = from_box(bx);
  assign xlimb = st[idx +: 8];
  assign oor = st[idx*40 +: 4];
  initial begin
    st = '0; idx = 0;
    for (int k = 0; k < 12; k++) begin
      st = {st[25*W-2:0], st[25*W-1] ^ st[7] ^ 1'b1} ^ (400'h1 << (k * 37));
      idx = k * 11;
      if (k == 5) st[100] = 1'bx;
      #1 $display("%0d %h %h %h %h", k, bx[2][3], back[63:0], xlimb, oor);
    end
  end
endmodule
