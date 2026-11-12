// IEEE 1800-2023 37.32 to 37.51: processes (vpiInitial, vpiAlways with
// vpiAlwaysType, vpiFinal) and continuous assignments of a module, their
// statements (begin / fork, assignment, if, case with its items, loops,
// delay and event controls, task calls, wait, disable, force / release)
// and expressions (vpiOperation with vpiOpType and operands, vpiConstant
// with vpiConstType and value, references to nets and variables, selects,
// function calls), walked from vpi_iterate(vpiProcess, module).
//! icarus-skip: Icarus 13 gives a different object model for SystemVerilog processes
//! args: data/vpi/statements.c
module top;
  logic [7:0] a, b = 8'h0f, mem [0:3];
  logic clk = 0;
  int i;
  wire [7:0] w;
  assign #2 w = a & ~b;
  function automatic int twice(int x); return 2 * x; endfunction
  task automatic show(input int v); $display("v %0d", v); endtask
  always_ff @(posedge clk) a <= b + 8'd1;
  always_comb begin
    unique case (b[1:0])
      2'b00, 2'b01: i = 1;
      default: i = twice(3);
    endcase
  end
  initial begin : main
    #1 clk = 1;
    for (int k = 0; k < 2; k++) mem[k] = 8'hA5;
    if (a == 8'h10) show(a); else show(-1);
    fork
      repeat (2) #1;
    join_none
    wait (i != 0) force b = 8'h3c;
    release b;
    $walk;
  end
  final $display("done");
endmodule
//> v -1
//> vpiContAssign line 15
//>   lhs: vpiNet line 14 w
//>   rhs: vpiOperation line 15 op 28 `a & ~b`
//>     vpiReg line 11 a
//>     vpiOperation line 15 op 4 `~b`
//>       vpiReg line 11 b
//>   delay: vpiConstant line 15 const type 1 value 2 size 32 `2`
//> vpiAlways line 18 type 3
//>   stmt: vpiEventControl line 18
//>     event: vpiReg line 12 clk
//>     stmt: vpiAssignment line 18 blocking 0
//>       lhs: vpiReg line 11 a
//>       rhs: vpiOperation line 18 op 24 `b + 8'd1`
//>         vpiReg line 11 b
//>         vpiConstant line 18 const type 1 value 1 size 8 `8'd1`
//> vpiAlways line 19 type 2
//>   stmt: vpiBegin line 20
//>     vpiCase line 20 case type 1 qualifier 1
//>       cond: vpiPartSelect line 0 b[1:0]
//>       item: vpiCaseItem line 21
//>         expr: vpiConstant line 21 const type 3 value 0 size 2 `2'b00`
//>         expr: vpiConstant line 21 const type 3 value 1 size 2 `2'b01`
//>         stmt: vpiAssignment line 21 blocking 1
//>           lhs: vpiIntVar line 13 i
//>           rhs: vpiConstant line 21 const type 1 value 1 size 32 `1`
//>       item: vpiCaseItem line 22
//>         stmt: vpiAssignment line 22 blocking 1
//>           lhs: vpiIntVar line 13 i
//>           rhs: vpiFuncCall line 22 twice `twice(3)`
//>             arg: vpiConstant line 22 const type 1 value 3 size 32 `3`
//> vpiInitial line 25
//>   stmt: vpiNamedBegin line 25 main
//>     vpiDelayControl line 26
//>       delay: vpiConstant line 26 const type 1 value 1 size 32 `1`
//>       stmt: vpiAssignment line 26 blocking 1
//>         lhs: vpiReg line 12 clk
//>         rhs: vpiConstant line 26 const type 1 value 1 size 32 `1`
//>     vpiFor line 27
//>       init: vpiAssignment line 27 blocking 1
//>         lhs: vpiIntVar line 27 k
//>         rhs: vpiConstant line 27 const type 1 value 0 size 32 `0`
//>       cond: vpiOperation line 27 op 20 `k < 2`
//>         vpiIntVar line 27 k
//>         vpiConstant line 27 const type 1 value 2 size 32 `2`
//>       inc: vpiAssignment line 27 blocking 1
//>         lhs: vpiIntVar line 27 k
//>         rhs: vpiOperation line 27 op 62 `k++`
//>           vpiIntVar line 27 k
//>       body: vpiAssignment line 27 blocking 1
//>         lhs: vpiBitSelect line 27 `mem[k]`
//>           of: vpiRegArray line 11 mem
//>           index: vpiIntVar line 27 k
//>         rhs: vpiConstant line 27 const type 5 value 165 size 8 `8'hA5`
//>     vpiIfElse line 28
//>       cond: vpiOperation line 28 op 14 `a == 8'h10`
//>         vpiReg line 11 a
//>         vpiConstant line 28 const type 5 value 16 size 8 `8'h10`
//>       then: vpiTaskCall line 28 show
//>         arg: vpiReg line 11 a
//>       else: vpiTaskCall line 28 show
//>         arg: vpiOperation line 28 op 1 `-1`
//>           vpiConstant line 28 const type 1 value 1 size 32 `1`
//>     vpiFork line 30 join 1
//>       vpiRepeat line 30
//>         cond: vpiConstant line 30 const type 1 value 2 size 32 `2`
//>         body: vpiDelayControl line 30
//>           delay: vpiConstant line 30 const type 1 value 1 size 32 `1`
//>           stmt: vpiNullStmt line 30
//>     vpiWait line 32
//>       cond: vpiOperation line 32 op 15 `i != 0`
//>         vpiIntVar line 13 i
//>         vpiConstant line 32 const type 1 value 0 size 32 `0`
//>       body: vpiForce line 32
//>         lhs: vpiReg line 11 b
//>         rhs: vpiConstant line 32 const type 5 value 60 size 8 `8'h3c`
//>     vpiRelease line 33
//>       lhs: vpiReg line 11 b
//>     vpiSysTaskCall line 34 $walk
//> vpiFinal line 36
//>   stmt: vpiSysTaskCall line 36 $display
//>     arg: vpiConstant line 36 const type 6 value 1685024357 size 32 `"done"`
//> module of the first statement: top
//> done
