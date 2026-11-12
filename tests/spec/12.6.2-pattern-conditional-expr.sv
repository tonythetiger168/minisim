// IEEE 1800-2023 12.6.2, 12.6.3: pattern matching in a conditional
// expression (x matches p &&& g ? a : b), with tagged, wildcard and
// variable patterns and a guard that reads the bound variable; and real
// and string constant patterns in case ... matches and if ... matches.
//! icarus-skip: Icarus 13 has no tagged unions or pattern matching
module top;
  typedef union tagged { void Invalid; int Valid; } VInt;
  VInt v;
  string s = "go";
  real r = 2.5;
  int y;
  initial begin
    v = tagged Valid 7;
    y = v matches tagged Valid .n ? 1 : 0;
    $display("%0d", y);
    y = v matches tagged Valid .n &&& n > 5 ? n * 2 : -1;
    $display("%0d", y);
    y = v matches tagged Valid .n &&& n > 9 ? n * 2 : -1;
    $display("%0d", y);
    $display("%s", v matches tagged Valid .* ? "valid" : "invalid");
    v = tagged Invalid;
    $display("%0d", v matches tagged Valid .* ? 1 : 2);
    $display("%f", v matches tagged Valid .* ? 0.5 : 1.5);
    case (s) matches
      "stop": $display("stop");
      "go": $display("go");
    endcase
    case (r) matches
      1.0: $display("one");
      2.5: $display("two and a half");
    endcase
    if (r matches 2.5 &&& s == "go") $display("both");
  end
endmodule
//> 1
//> 14
//> -1
//> valid
//> 2
//> 1.500000
//> go
//> two and a half
//> both
