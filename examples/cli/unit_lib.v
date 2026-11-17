// A library file (-v, 33.3.1): read after the sources in their compilation
// unit, so it sees their macros and $unit declarations, and only the modules
// something instantiates are elaborated.
module unit_cell;
  initial #1 $display("cell w=%0d twice=%0d", `UNIT_W, unit_twice(`UNIT_W));
endmodule
module unit_unused;
  initial $display(no_such_scope.sig);
  initial $no_such_task(1);
endmodule
