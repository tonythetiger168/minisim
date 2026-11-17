// A library file (-v, 33.3.1): it sees the macros of the files before it,
// and only the modules something instantiates are elaborated.
module unit_cell;
  initial #1 $display("cell w=%0d", `UNIT_W);
endmodule
module unit_unused;
  initial $display(no_such_scope.sig);
endmodule
