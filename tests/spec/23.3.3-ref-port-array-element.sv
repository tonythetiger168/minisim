// 23.3.3: a ref port connected to an element of a fixed-size unpacked array
// (constant indices, also of a multidimensional array) is that element: a
// write on either side is seen at once on the other.
//! icarus-skip: Icarus does not support ref ports
module child(ref logic [7:0] r);
  initial #1 r = r + 8'd5;
  initial #3 $display("child sees %0d", r);
endmodule
module top;
  logic [7:0] mem[4];
  logic [7:0] grid[2][3];
  child c0(.r(mem[2]));
  child c1(.r(grid[1][0]));
  initial begin
    mem[2] = 10;
    grid[1][0] = 20;
    #2 $display("%0d %0d", mem[2], grid[1][0]);
    mem[2] = 100;
  end
endmodule
//> 15 25
//> child sees 100
//> child sees 25
