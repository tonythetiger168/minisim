// The testbench side of examples/pss/dma.pss for the UVM output: a memory
// and a DMA model behind the tasks of the generated API class. A copy takes
// time, so the copies of a parallel block overlap, and two of them on one
// channel would be reported.
`include "uvm_macros.svh"
module top;
  import uvm_pkg::*;
  import entry_pkg::*;

  class dma_api extends entry_api;
    bit [7:0] mem [int];
    bit busy [4];
    function new(string name = "dma_api"); super.new(name); endfunction
    virtual task mem_fill(input bit [31:0] addr, input bit [15:0] size, input bit [7:0] seed);
      for (int i = 0; i < size; i++) mem[addr + i] = seed + i;
      #1;
    endtask
    virtual task dma_copy(input bit [7:0] chan, input bit [31:0] src, input bit [31:0] dst, input bit [15:0] size);
      if (busy[chan]) `uvm_error("DMA", $sformatf("channel %0d is already busy", chan))
      busy[chan] = 1;
      #(size);
      for (int i = 0; i < size; i++) mem[dst + i] = mem[src + i];
      busy[chan] = 0;
    endtask
    virtual task mem_sum(input bit [31:0] addr, input bit [15:0] size, output bit [31:0] mpss_ret);
      mpss_ret = 0;
      for (int i = 0; i < size; i++) mpss_ret += mem[addr + i];
      mpss_ret |= 1;
      #1;
    endtask
  endclass

  class dma_test extends uvm_test;
    `uvm_component_utils(dma_test)
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    task run_phase(uvm_phase phase);
      dma_api api = new();
      entry_base_seq seq;
      phase.raise_objection(this);
      uvm_config_db#(entry_api)::set(null, "*", "mpss_api", api);
      // Every scenario, by name through the factory.
      for (int i = 0; i < 12; i++) begin
        $cast(seq, uvm_factory::get().create_object_by_name($sformatf("entry_%0d_seq", i)));
        seq.start(null);
      end
      $display("done at %0t", $time);
      phase.drop_objection(this);
    endtask
  endclass
  initial run_test("dma_test");
endmodule
