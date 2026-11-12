// IEEE 1800-2023 37.31: vpi_iterate(vpiConstraint, class definition) gives
// its constraint blocks (vpiName, vpiIsConstraintEnabled, vpiAutomatic: not
// static), and vpiConstraintItem their items: expressions, implications
// (vpiImplication), if and if-else (vpiConstrIf / vpiConstrIfElse with
// vpiCondition, vpiConstraintExpr and vpiElseConst), foreach
// (vpiConstrForEach), disable soft (vpiSoftDisable) and solve before
// (vpiConstraintOrdering with vpiSolveBefore / vpiSolveAfter).
//! icarus-skip: Icarus 13 has no SystemVerilog classes through VPI
//! args: data/vpi/constraints.c
module top;
  class Packet;
    rand bit [7:0] len;
    rand bit [1:0] kind;
    rand bit [3:0] data[4];
    constraint c_len { len > 2; len < 64; }
    constraint c_kind {
      kind == 0 -> len < 10;
      if (kind == 1) len > 20; else { len < 40; len != 30; }
      soft kind != 3;
      solve kind before len;
    }
    static constraint c_data { foreach (data[i]) data[i] < 8; }
    constraint c_off { disable soft kind; }
  endclass
  Packet p;
  initial begin
    p = new;
    $constraints;
  end
endmodule
//> vpiConstraint c_len enabled 1 virtual 0 automatic 1 of Packet
//>   vpiOperation len > 2
//>   vpiOperation len < 64
//> vpiConstraint c_kind enabled 1 virtual 0 automatic 1 of Packet
//>   vpiImplication if kind == 0
//>     vpiOperation len < 10
//>   vpiConstrIfElse if kind == 1
//>     vpiOperation len > 20
//>   else
//>     vpiOperation len < 40
//>     vpiOperation len != 30
//>   vpiOperation kind != 3
//>   vpiConstraintOrdering kind before len
//> vpiConstraint c_data enabled 1 virtual 0 automatic 0 of Packet
//>   vpiConstrForEach data
//>     vpiOperation data[i] < 8
//> vpiConstraint c_off enabled 1 virtual 0 automatic 1 of Packet
//>   vpiSoftDisable kind
