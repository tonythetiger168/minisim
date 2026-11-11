// IEEE 1800-2023 7.3.2, 11.9, 12.6: tagged unions are type-checked. A
// member is read with pattern matching (nested patterns, struct patterns,
// &&& filters, wildcards) or with .member; reading a member that the tag
// does not name is a run-time error.
//! icarus-skip: Icarus 13 does not support tagged unions
module top;
  typedef union tagged { void Invalid; int Valid; } VInt;
  typedef union tagged {
    struct { bit [4:0] r1, r2; } Add;
    union tagged { bit [9:0] JmpU; struct { bit [1:0] cc; bit [9:0] addr; } JmpC; } Jmp;
  } Instr;
  VInt v;
  Instr ins;
  initial begin
    v = tagged Valid 23;
    case (v) matches
      tagged Invalid: $display("inv");
      tagged Valid .n: $display("valid %0d", n);
    endcase
    if (v matches tagged Valid .n &&& n > 10) $display("big %0d", n);
    v = tagged Invalid;
    if (v matches tagged Invalid) $display("isinv");
    ins = tagged Add '{5'd3, 5'd4};
    case (ins) matches
      tagged Add '{.a, .b}: $display("add %0d %0d", a, b);
      tagged Jmp (tagged JmpU .x): $display("jmpu %0d", x);
      tagged Jmp (tagged JmpC '{.c, .ad}): $display("jmpc %0d %0d", c, ad);
    endcase
    ins = tagged Jmp (tagged JmpC '{2'd1, 10'd77});
    case (ins) matches
      tagged Add .*: $display("add");
      tagged Jmp (tagged JmpC '{cc: .c, addr: .ad}) &&& c == 1: $display("jmpc %0d %0d", c, ad);
      default: $display("other");
    endcase
    $display("%0d", ins.Jmp.JmpC.addr);
    v = tagged Valid 5;
    $display("%0d", v.Valid);
    v = tagged Invalid;
    $display("%0d", v.Valid);
  end
endmodule
//> valid 23
//> big 23
//> isinv
//> add 3 4
//> jmpc 1 77
//> 77
//> 5
//> Error: 7.3.2-tagged-union-check.sv:39: read of tagged union member 'Valid', which is not the member the tag names
//> 0
