// IEEE 1800-2023 37.16 details 14 and 15, 37.21: a force statement in
// effect drives the net or variable it forces; a procedural assign
// statement in effect drives the variable it assigns (a net has only force
// drivers). Both are loads of what their right side reads. Once released
// or deassigned, they are neither. A force from another module is not a
// local driver.
//! icarus-skip: Icarus 13 does not give force statements as drivers
//! args: data/vpi/holddrv.c
module top;
  wire n;
  logic r, src = 1;
  assign n = src;
  initial begin
    #1 $show;
    force n = src;
    assign r = ~src;
    #1 $show;
    release n;
    deassign r;
    #1 $show;
  end
endmodule
module tb;
  initial begin
    force top.r = 0;
    #4 release top.r;
  end
endmodule
//> time 1
//>   top.n drivers: vpiContAssign@12
//>   top.r drivers: vpiForce@25
//>   top.r local drivers:
//>   top.src loads: vpiContAssign@12
//> time 2
//>   top.n drivers: vpiForce@15 vpiContAssign@12
//>   top.r drivers: vpiAssignStmt@16 vpiForce@25
//>   top.r local drivers: vpiAssignStmt@16
//>   top.src loads: vpiForce@15 vpiAssignStmt@16 vpiContAssign@12
//> time 3
//>   top.n drivers: vpiContAssign@12
//>   top.r drivers: vpiForce@25
//>   top.r local drivers:
//>   top.src loads: vpiContAssign@12
