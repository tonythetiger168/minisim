// IEEE 1800-2023 35.5.3: A context import knows the instance it is called
// from: svGetScope and svGetNameFromScope, svGetCallerInfo, per-scope user
// data, and svSetScope / svGetScopeFromName to pick the instance whose
// export is called (35.5.3, H.9).
//! args: data/dpi/context.c
//! icarus-skip: Icarus 13 does not support DPI
module leaf;
  import "DPI-C" context function void where();
  import "DPI-C" context function int bump();
  int v = 0;
  function int sv_get(); return v; endfunction
  function void sv_set(int x); v = x; endfunction
  export "DPI-C" function sv_get;
  export "DPI-C" function sv_set;
  task show(); where(); endtask
endmodule

module top;
  import "DPI-C" context function void poke_all();
  leaf u0(), u1();
  initial begin
    u0.show();
    u1.show();
    $display("%0d %0d %0d", u0.bump(), u0.bump(), u1.bump());
    u0.v = 1; u1.v = 2;
    poke_all();
    $display("%0d %0d", u0.v, u1.v);
  end
endmodule

//> scope top.u0 called from 35.5.3-context.sv:15
//> scope top.u1 called from 35.5.3-context.sv:15
//> 1 2 1
//> 101 202
