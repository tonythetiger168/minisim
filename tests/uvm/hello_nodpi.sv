// UVM smoke test (IEEE 1800.2-2020) with the library built without its C
// code (msim -uvmnodpi, +define+UVM_NO_DPI): run_test, the factory, a task
// phase with an objection, and `uvm_info.
//! nodpi
`include "uvm_macros.svh"
module top;
  import uvm_pkg::*;
  class hello_test extends uvm_test;
    `uvm_component_utils(hello_test)
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    task run_phase(uvm_phase phase);
      phase.raise_objection(this);
      `uvm_info("TEST", "hello from UVM", UVM_LOW)
      #10;
      phase.drop_objection(this);
    endtask
  endclass
  initial run_test("hello_test");
endmodule

//> UVM_INFO hello_nodpi.sv(13) @ 0: uvm_test_top [TEST] hello from UVM
//> UVM_ERROR :    0
//> UVM_FATAL :    0
