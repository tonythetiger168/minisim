// IEEE 1800-2023 8.25: a class parameter or enum value read through a handle
// is a constant, so a bit or part select of it works like any constant.
class C;
  localparam logic [3:0] X = 4'b0101;
  typedef enum {A, B, D} e_t;
  int v;
endclass
module t;
  C c = new;
  int k = 2;
  initial begin
    $display("%b %b %b %0d", c.X[0], c.X[k], c.X[3:2], c.D);
  end
endmodule
//> 1 1 01 2
