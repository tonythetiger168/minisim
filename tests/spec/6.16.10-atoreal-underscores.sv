// 6.16.10: str.atoreal() reads the longest prefix that is a real constant
// (5.7.2), so underscores between digits are skipped; 0 without digits.
//! icarus-skip: Icarus 13 stops atoreal at the first underscore
module top;
  string s;
  initial begin
    s = "1_234.56";    $display("%f", s.atoreal());
    s = "1_000_000.5"; $display("%f", s.atoreal());
    s = "2.5e1_0x";    $display("%g", s.atoreal());
    s = "3e";          $display("%g", s.atoreal());
    s = "_1";          $display("%g", s.atoreal());
    s = "-4.25";       $display("%g", s.atoreal());
  end
endmodule
//> 1234.560000
//> 1000000.500000
//> 2.5e+10
//> 3
//> 0
//> -4.25
