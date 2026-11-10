// IEEE 1800-2023 7.3.2: Tagged unions (unpacked and packed, void members, a
// struct member), tagged expressions (11.9), and pattern matching (12.6): if
// matches with &&& guards, case/casez matches with variable, wildcard,
// constant and structure patterns.
//! icarus-skip: Icarus 13 does not support tagged unions
module t;
  typedef union tagged { void Invalid; int Valid; } vint;
  typedef union tagged packed { void None; bit [7:0] B; bit [3:0] N; } pu;
  typedef struct { int a; int b; } ab_t;
  typedef union tagged { ab_t P; real R; bit [3:0] K; } ru;
  vint x; pu p; ru r;
  int hits;
  initial begin
    x = tagged Invalid;
    if (x matches tagged Valid .n) $display("valid %0d", n); else $display("invalid");
    x = tagged Valid 42;
    if (x matches tagged Valid .n &&& n > 40) $display("big %0d", n);
    if (x matches tagged Valid 42) $display("is 42");
    p = tagged B 8'hA5;
    $display("%0d %h", $bits(p), p);
    case (p) matches
      tagged None: $display("none");
      tagged N .k: $display("N %h", k);
      tagged B .b &&& b[0]: $display("odd B %h", b);
      default: $display("other");
    endcase
    r = tagged P '{3, 4};
    case (r) matches
      tagged R .f: $display("R %f", f);
      tagged P '{.a, 4}: $display("P a=%0d b=4", a);
      tagged P '{.*, .bb}: $display("P b=%0d", bb);
    endcase
    r = tagged R 2.5;
    if (r matches tagged R .f) $display("R %f", f);
    r = tagged K 4'b1x0z;
    casez (r) matches
      tagged K 4'b1?0?: $display("casez hit");
      default: $display("casez miss");
    endcase
    $display("%0d", x.Valid);
  end
endmodule
//> invalid
//> big 42
//> is 42
//> 10 1a5
//> odd B a5
//> P a=3 b=4
//> R 2.500000
//> casez hit
//> 42
