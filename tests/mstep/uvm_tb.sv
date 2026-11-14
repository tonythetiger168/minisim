// vlog with no UVM option: Questa finds its precompiled UVM; msim uses its
// own (-uvm) for a file that imports uvm_pkg (tests/msim_mstep.cmake).
`include "uvm_macros.svh"
module uvm_tb;
  import uvm_pkg::*;
  initial `uvm_info("MSTEP", "hello from uvm", UVM_NONE)
endmodule
