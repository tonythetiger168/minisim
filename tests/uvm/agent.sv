// UVM agent testbench (IEEE 1800.2-2020): a sequence, sequencer, driver,
// monitor and scoreboard around an adder, a virtual interface passed with
// uvm_config_db, an analysis port, and the phases from build to report.
`include "uvm_macros.svh"
interface adder_if(input logic clk);
  logic [7:0] a, b;
  logic [8:0] sum;
  logic valid;
endinterface

module adder(adder_if vif);
  always @(posedge vif.clk) if (vif.valid) vif.sum <= vif.a + vif.b;
endmodule

package tb_pkg;
  import uvm_pkg::*;
  class txn extends uvm_sequence_item;
    rand bit [7:0] a, b;
    bit [8:0] sum;
    constraint c_small { a < 200; }
    `uvm_object_utils_begin(txn)
      `uvm_field_int(a, UVM_ALL_ON)
      `uvm_field_int(b, UVM_ALL_ON)
      `uvm_field_int(sum, UVM_ALL_ON)
    `uvm_object_utils_end
    function new(string name = "txn"); super.new(name); endfunction
  endclass

  class seq extends uvm_sequence #(txn);
    `uvm_object_utils(seq)
    int n = 5;
    function new(string name = "seq"); super.new(name); endfunction
    task body();
      repeat (n) begin
        txn t = txn::type_id::create("t");
        start_item(t);
        if (!t.randomize()) `uvm_error("SEQ", "randomize failed")
        finish_item(t);
      end
    endtask
  endclass

  class drv extends uvm_driver #(txn);
    `uvm_component_utils(drv)
    virtual adder_if vif;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    function void build_phase(uvm_phase phase);
      if (!uvm_config_db#(virtual adder_if)::get(this, "", "vif", vif)) `uvm_fatal("NOVIF", "no vif")
    endfunction
    task run_phase(uvm_phase phase);
      vif.valid <= 0;
      forever begin
        seq_item_port.get_next_item(req);
        @(negedge vif.clk);
        vif.a <= req.a; vif.b <= req.b; vif.valid <= 1;
        @(negedge vif.clk);
        vif.valid <= 0;
        seq_item_port.item_done();
      end
    endtask
  endclass

  class mon extends uvm_monitor;
    `uvm_component_utils(mon)
    virtual adder_if vif;
    uvm_analysis_port #(txn) ap;
    function new(string name, uvm_component parent); super.new(name, parent); ap = new("ap", this); endfunction
    function void build_phase(uvm_phase phase);
      void'(uvm_config_db#(virtual adder_if)::get(this, "", "vif", vif));
    endfunction
    task run_phase(uvm_phase phase);
      forever begin
        @(posedge vif.clk);
        if (vif.valid) begin
          txn t = txn::type_id::create("t");
          t.a = vif.a; t.b = vif.b;
          @(negedge vif.clk);
          t.sum = vif.sum;
          ap.write(t);
        end
      end
    endtask
  endclass

  class sb extends uvm_scoreboard;
    `uvm_component_utils(sb)
    uvm_analysis_imp #(txn, sb) imp;
    int pass, fail;
    function new(string name, uvm_component parent); super.new(name, parent); imp = new("imp", this); endfunction
    function void write(txn t);
      if (t.sum == t.a + t.b) pass++;
      else begin fail++; `uvm_error("SB", $sformatf("mismatch %s", t.sprint())) end
    endfunction
    function void report_phase(uvm_phase phase);
      `uvm_info("SB", $sformatf("pass=%0d fail=%0d", pass, fail), UVM_NONE)
    endfunction
  endclass

  class agent extends uvm_agent;
    `uvm_component_utils(agent)
    drv d; mon m; uvm_sequencer #(txn) sqr;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    function void build_phase(uvm_phase phase);
      d = drv::type_id::create("d", this);
      m = mon::type_id::create("m", this);
      sqr = uvm_sequencer#(txn)::type_id::create("sqr", this);
    endfunction
    function void connect_phase(uvm_phase phase);
      d.seq_item_port.connect(sqr.seq_item_export);
    endfunction
  endclass

  class env extends uvm_env;
    `uvm_component_utils(env)
    agent ag; sb s;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    function void build_phase(uvm_phase phase);
      ag = agent::type_id::create("ag", this);
      s = sb::type_id::create("s", this);
    endfunction
    function void connect_phase(uvm_phase phase);
      ag.m.ap.connect(s.imp);
    endfunction
  endclass

  class test extends uvm_test;
    `uvm_component_utils(test)
    env e;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    function void build_phase(uvm_phase phase);
      e = env::type_id::create("e", this);
    endfunction
    task run_phase(uvm_phase phase);
      seq s = seq::type_id::create("s");
      phase.raise_objection(this);
      s.start(e.ag.sqr);
      #20;
      phase.drop_objection(this);
    endtask
  endclass
endpackage

module top;
  import uvm_pkg::*;
  import tb_pkg::*;
  logic clk = 0;
  always #5 clk = ~clk;
  adder_if aif(clk);
  adder dut(aif);
  initial begin
    uvm_config_db#(virtual adder_if)::set(null, "*", "vif", aif);
    run_test("test");
  end
endmodule

//> UVM_INFO agent.sv(95) @ 120000: uvm_test_top.e.s [SB] pass=5 fail=0
//> UVM_ERROR :    0
//> UVM_FATAL :    0
