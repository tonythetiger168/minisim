// UVM features (IEEE 1800.2-2020): field macros with print / clone /
// compare / sprint / pack, a factory type override, a TLM FIFO between a put
// port and a get port, a report catcher that demotes an error, print_topology,
// randomize() with, and +UVM_TESTNAME.
//! args: +UVM_TESTNAME=t
`include "uvm_macros.svh"
package p5;
  import uvm_pkg::*;
  typedef enum {RED, GREEN, BLUE} color_e;
  class item extends uvm_sequence_item;
    rand int unsigned addr;
    rand bit [15:0] data;
    color_e color;
    string tag;
    int q[$];
    `uvm_object_utils_begin(item)
      `uvm_field_int(addr, UVM_ALL_ON | UVM_HEX)
      `uvm_field_int(data, UVM_ALL_ON)
      `uvm_field_enum(color_e, color, UVM_ALL_ON)
      `uvm_field_string(tag, UVM_ALL_ON)
      `uvm_field_queue_int(q, UVM_ALL_ON)
    `uvm_object_utils_end
    function new(string name = "item"); super.new(name); endfunction
  endclass
  class item2 extends item;
    `uvm_object_utils(item2)
    function new(string name = "item2"); super.new(name); endfunction
  endclass

  class prod extends uvm_component;
    `uvm_component_utils(prod)
    uvm_blocking_put_port #(item) put;
    function new(string n, uvm_component p); super.new(n, p); put = new("put", this); endfunction
    task run_phase(uvm_phase phase);
      phase.raise_objection(this);
      for (int i = 0; i < 3; i++) begin
        item it = item::type_id::create($sformatf("it%0d", i));
        it.addr = i * 16; it.data = i + 100;
        #5 put.put(it);
      end
      phase.drop_objection(this);
    endtask
  endclass
  class cons extends uvm_component;
    `uvm_component_utils(cons)
    uvm_blocking_get_port #(item) get;
    int got;
    function new(string n, uvm_component p); super.new(n, p); get = new("get", this); endfunction
    task run_phase(uvm_phase phase);
      forever begin
        item it;
        get.get(it);
        got++;
        `uvm_info("CONS", $sformatf("got %s addr=%0d type=%s", it.get_name(), it.addr, it.get_type_name()), UVM_MEDIUM)
      end
    endtask
  endclass

  class catcher extends uvm_report_catcher;
    function new(string name = "catcher"); super.new(name); endfunction
    function action_e catch();
      if (get_id() == "DEMOTE") begin set_severity(UVM_INFO); return THROW; end
      return THROW;
    endfunction
  endclass

  class t extends uvm_test;
    `uvm_component_utils(t)
    prod pr; cons co; uvm_tlm_fifo #(item) f;
    function new(string n, uvm_component p); super.new(n, p); endfunction
    function void build_phase(uvm_phase phase);
      item::type_id::set_type_override(item2::get_type());
      pr = prod::type_id::create("pr", this);
      co = cons::type_id::create("co", this);
      f = new("f", this);
    endfunction
    function void connect_phase(uvm_phase phase);
      pr.put.connect(f.put_export);
      co.get.connect(f.get_export);
    endfunction
    function void end_of_elaboration_phase(uvm_phase phase);
      uvm_top.print_topology();
    endfunction
    task run_phase(uvm_phase phase);
      item a, b;
      catcher c = new;
      uvm_report_cb::add(null, c);
      a = item::type_id::create("a");
      a.addr = 'h1234; a.data = 7; a.color = BLUE; a.tag = "hello"; a.q = '{1, 2, 3};
      a.print();
      $cast(b, a.clone());
      `uvm_info("CMP", $sformatf("compare=%0d", a.compare(b)), UVM_NONE)
      b.data = 8;
      `uvm_info("CMP", $sformatf("compare after change=%0d", a.compare(b)), UVM_NONE)
      `uvm_info("STR", b.sprint(uvm_default_line_printer), UVM_NONE)
      `uvm_error("DEMOTE", "this error is demoted by a catcher")
      begin
        bit [7:0] bytes[];
        int n = a.pack_bytes(bytes);
        `uvm_info("PACK", $sformatf("packed %0d bits", n), UVM_NONE)
      end
      for (int i = 0; i < 3; i++) begin
        bit ok = a.randomize() with { addr < 10; };
        `uvm_info("RAND", $sformatf("ok=%0d addr=%0d", ok, a.addr), UVM_NONE)
      end
    endtask
    function void report_phase(uvm_phase phase);
      `uvm_info("DONE", $sformatf("consumer got %0d", co.got), UVM_NONE)
    endfunction
  endclass
endpackage
module top;
  import uvm_pkg::*;
  import p5::*;
  initial run_test();
endmodule

//>   uvm_test_top         t                      -     @
//>   tag    string            5     hello 
//> [CMP] compare=1
//> Miscompare for a.data: lhs = 'h7 : rhs = 'h8
//> [CMP] compare after change=0
//> { addr: 'h1234  data: 'h8  color: BLUE  tag: hello  q: { [0]: 'h1  [1]: 'h2  [2]: 'h3  } }
//> UVM_INFO features.sv(96) @ 0: uvm_test_top [DEMOTE] this error is demoted by a catcher
//> @ 5000: uvm_test_top.co [CONS] got it0 addr=0 type=item2
//> @ 15000: uvm_test_top.co [CONS] got it2 addr=32 type=item2
//> [DONE] consumer got 3
//> Number of demoted UVM_ERROR reports  :    1
//> UVM_ERROR :    0
//> UVM_FATAL :    0
