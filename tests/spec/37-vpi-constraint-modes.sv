// IEEE 1800-2023 37.31, 18.5.3, 18.5.13, 18.9: vpiSoft on a soft expression
// constraint (0 on a hard one); a dist constraint is a vpiDistribution with
// vpiExpr and vpiDistItem items, each with vpiDistType (:= vpiEqualDist,
// :/ vpiDivDist), vpiWeight and vpiValueRange (a value, or a range object
// with vpiLeftRange / vpiRightRange). vpi_iterate(vpiConstraint, class
// object) gives the object's blocks, its own then the inherited ones it does
// not override; their vpiIsConstraintEnabled follows constraint_mode.
//! icarus-skip: Icarus 13 has no SystemVerilog classes through VPI
//! args: data/vpi/constrmode.c
module top;
  class Base;
    rand int a;
    constraint ca { a < 10; }
    constraint cbase { a >= 0; }
  endclass
  class Pkt extends Base;
    rand int b;
    constraint cb { soft b == 3; a dist {1 := 2, [4:5] :/ 3}; }
    constraint ca { a > 0; }
  endclass
  Pkt p;
  initial begin
    p = new;
    $show;
    p.cb.constraint_mode(0);
    p.cbase.constraint_mode(0);
    $show;
  end
endmodule
//> class cb: soft 1, then soft 0 vpiDistribution of a
//> item vpiEqualDist value 1 weight 2
//> item vpiDivDist range 4:5 weight 3
//> object: cb 1, ca 1, cbase 1, parent is object 1
//> object: cb 0, ca 1, cbase 0, parent is object 1
