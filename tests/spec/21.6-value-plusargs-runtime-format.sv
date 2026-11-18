// IEEE 1800-2023 21.6: the user string of $value$plusargs may be any string
// expression, so the prefix and the format letter can be known only at run
// time; the value is read as that letter says.
//! args: +HEX=ff0 +NUM=42 +NAME=abc +R=2.5
module t;
  reg [8*8:1] f;
  string s;
  logic [15:0] v;
  int n;
  real r;
  initial begin
    f = "HEX=%h";
    if ($value$plusargs(f, v)) $display("%h", v);
    s = "NUM=%d";
    if ($value$plusargs(s, n)) $display("%0d", n);
    s = "NAME=%s";
    if ($value$plusargs(s, f)) $display("%0s", f);
    s = "R=%f";
    if ($value$plusargs(s, r)) $display("%0.2f", r);
    s = "MISSING=%d";
    n = 7;
    $display("%0d %0d", $value$plusargs(s, n), n);
  end
endmodule
//> 0ff0
//> 42
//> abc
//> 2.50
//> 0 7
