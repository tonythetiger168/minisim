// IEEE 1800-2023 8.25 Parameterized classes, 6.20 Constants: a parameter or
// localparam of a class read through a handle is its constant value.
//! icarus-skip: Icarus 13 does not read a class parameter through a handle
class regs #(int W = 8);
  localparam int N_REGS = 64;
  localparam string NAME = "regs";
  int x = W + N_REGS;
endclass
module t;
  initial begin
    regs #(16) r = new;
    int n;
    n += r.N_REGS;
    $display("%0d %0d %0d %s", n, r.W, r.x, r.NAME);
  end
endmodule
//> 64 16 80 regs
