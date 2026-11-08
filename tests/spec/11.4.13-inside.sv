//! icarus-skip: Icarus 13 has no inside, and no assignment inside an expression
// IEEE 1800-2023 11.4.13: inside matches values and [lo:hi] ranges with ==?, so an x
// in an item matches anything and an x on the left gives x when nothing matches.
// 11.3.6: an assignment in parentheses is an expression.
module top;
  logic [7:0] v;
  integer i = 2, j;
  initial begin
    v = 8'd7;
    $display("%0d %0d %0d", v inside {[1:5], 7}, i inside {1, 3}, 4'b1010 inside {4'b1x1x});
    $display("%b", 4'b10x0 inside {4'b1000});
    $display("%b", 4'b10x0 inside {4'b1000, 4'b1010});
    if ((j = 7) == 7) $display("j=%0d", j);
  end
endmodule

//> 1 0 1
//> x
//> x
//> j=7
