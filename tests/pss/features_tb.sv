// The testbench side of features.pss for the UVM output. The tasks take no
// time, so the output is line for line what the C test prints.
`include "uvm_macros.svh"
module top;
  import uvm_pkg::*;
  import feat_pkg::*;

  class my_api extends feat_api;
    function new(string name = "my_api"); super.new(name); endfunction
    virtual task reg_write(input bit [31:0] addr, input bit [31:0] data);
      $display("write %0x %0x", addr, data);
    endtask
    virtual task reg_read(input bit [31:0] addr, output bit [31:0] mpss_ret);
      mpss_ret = addr ^ 32'h5a;
    endtask
  endclass

  class feat_test extends uvm_test;
    `uvm_component_utils(feat_test)
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    task run_phase(uvm_phase phase);
      my_api api = new();
      feat_base_seq seq;
      phase.raise_objection(this);
      // Every scenario, by name through the factory.
      for (int i = 0; i < 3; i++) begin
        $cast(seq, uvm_factory::get().create_object_by_name($sformatf("feat_%0d_seq", i)));
        seq.api = api;
        seq.start(null);
      end
      phase.drop_objection(this);
    endtask
  endclass
  initial run_test("feat_test");
endmodule
