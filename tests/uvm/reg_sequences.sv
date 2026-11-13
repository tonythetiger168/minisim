// The built-in register and memory test sequences of IEEE 1800.2-2020
// Annex E that had no check of their own: uvm_reg_access_seq (E.3), the
// shared access sequences (E.4), uvm_mem_access_seq (E.5),
// uvm_reg_mem_hdl_paths_seq (E.7) and uvm_reg_mem_built_in_seq (E.8).
// Each one runs clean on a good DUT and reports an error on a broken one.
//! args: +UVM_NO_RELNOTES
`include "uvm_macros.svh"
module dut;
  reg [31:0] r0 = 0, r1 = 0;
  reg [31:0] m [0:7];
  bit stuck, memfault;   // faults: r1[0] and m[5][4] do not take a write
  int nwr, nrd;
  function automatic void wr(input bit [31:0] a, input bit [31:0] d);
    nwr++;
    if (a >= 'h1000) a -= 'h1000;   // the second map
    case (a)
      'h00: r0 = d;
      'h04: r1 = stuck ? {d[31:1], r1[0]} : d;
      default: if (a >= 'h100 && a < 'h120) begin
        if (memfault && a == 'h114) d[4] = m[5][4];
        m[(a - 'h100) >> 2] = d;
      end
    endcase
  endfunction
  function automatic bit [31:0] rd(input bit [31:0] a);
    nrd++;
    if (a >= 'h1000) a -= 'h1000;
    case (a)
      'h00: return r0;
      'h04: return r1;
      default: if (a >= 'h100 && a < 'h120) return m[(a - 'h100) >> 2];
    endcase
    return 0;
  endfunction
  initial foreach (m[i]) m[i] = 0;
endmodule
module top;
  import uvm_pkg::*;
  dut u();
  int fails = 0;
  `define CHK(id, c) if (c) $display("ok %s", id); else begin $display("FAIL %s", id); fails++; end

  class bus_tx extends uvm_sequence_item;
    rand bit [31:0] addr, data; rand bit wr;
    `uvm_object_utils(bus_tx)
    function new(string name = "bus_tx"); super.new(name); endfunction
  endclass
  class adapter extends uvm_reg_adapter;
    `uvm_object_utils(adapter)
    function new(string name = "adapter"); super.new(name); endfunction
    virtual function uvm_sequence_item reg2bus(const ref uvm_reg_bus_op rw);
      bus_tx b = bus_tx::type_id::create("b"); b.addr = rw.addr; b.data = rw.data; b.wr = rw.kind == UVM_WRITE; return b;
    endfunction
    virtual function void bus2reg(uvm_sequence_item bus_item, ref uvm_reg_bus_op rw);
      bus_tx b; void'($cast(b, bus_item)); rw.kind = b.wr ? UVM_WRITE : UVM_READ; rw.addr = b.addr; rw.data = b.data; rw.status = UVM_IS_OK;
    endfunction
  endclass
  class drv extends uvm_driver #(bus_tx);
    `uvm_component_utils(drv)
    int n;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    task run_phase(uvm_phase phase);
      forever begin
        seq_item_port.get_next_item(req); #2; n++;
        if (req.wr) u.wr(req.addr, req.data); else req.data = u.rd(req.addr);
        seq_item_port.item_done();
      end
    endtask
  endclass
  class reg32 extends uvm_reg;
    rand uvm_reg_field f;
    `uvm_object_utils(reg32)
    function new(string name = "reg32"); super.new(name, 32, UVM_NO_COVERAGE); endfunction
    virtual function void build();
      f = uvm_reg_field::type_id::create("f"); f.configure(this, 32, 0, "RW", 0, 32'h0, 1, 1, 1);
    endfunction
  endclass
  class blk extends uvm_reg_block;
    rand reg32 r0, r1; uvm_mem m; uvm_reg_map map2;
    `uvm_object_utils(blk)
    function new(string name = "blk"); super.new(name, UVM_NO_COVERAGE); endfunction
    virtual function void build();
      r0 = reg32::type_id::create("r0"); r0.configure(this, null, "r0"); r0.build();
      r1 = reg32::type_id::create("r1"); r1.configure(this, null, "r1"); r1.build();
      m = new("m", 8, 32, "RW", UVM_NO_COVERAGE); m.configure(this, "m");
      default_map = create_map("map", 0, 4, UVM_LITTLE_ENDIAN);
      map2 = create_map("map2", 'h1000, 4, UVM_LITTLE_ENDIAN);
      default_map.add_reg(r0, 'h0, "RW"); default_map.add_reg(r1, 'h4, "RW"); default_map.add_mem(m, 'h100, "RW");
      map2.add_reg(r0, 'h0, "RW"); map2.add_reg(r1, 'h4, "RW"); map2.add_mem(m, 'h100, "RW");
      add_hdl_path("top.u");
      lock_model();
    endfunction
  endclass
  // counts the errors the sequences report, and keeps them out of the totals
  class err_catch extends uvm_report_catcher;
    int n; bit on;
    function new(string name = "err_catch"); super.new(name); endfunction
    virtual function action_e catch();
      if (on && get_severity() == UVM_ERROR) begin n++; return CAUGHT; end
      return THROW;
    endfunction
  endclass

  class test extends uvm_test;
    `uvm_component_utils(test)
    blk b; drv d; uvm_sequencer #(bus_tx) s; adapter ad; err_catch ec;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    function void build_phase(uvm_phase phase);
      b = blk::type_id::create("b"); b.build();
      d = drv::type_id::create("d", this); s = new("s", this); ad = adapter::type_id::create("ad");
      ec = new; uvm_report_cb::add(null, ec);
    endfunction
    function void connect_phase(uvm_phase phase);
      d.seq_item_port.connect(s.seq_item_export);
      b.default_map.set_sequencer(s, ad); b.default_map.set_auto_predict(1);
      b.map2.set_sequencer(s, ad); b.map2.set_auto_predict(1);
    endfunction
    // Runs seq on the model; returns the number of bus transfers. errs: errors it reported.
    task automatic go(uvm_reg_sequence #(uvm_sequence #(uvm_reg_item)) seq, output int ops, output int errs);
      int n0 = d.n;
      ec.n = 0; ec.on = 1;
      seq.model = b;
      seq.start(null);
      ec.on = 0;
      ops = d.n - n0; errs = ec.n;
    endtask
    task run_phase(uvm_phase phase);
      int ops, errs;
      phase.raise_objection(this);
      b.reset();
      begin   // E.3
        uvm_reg_access_seq q = uvm_reg_access_seq::type_id::create("q");
        go(q, ops, errs);
        `CHK("E.3 reg_access_seq", errs == 0 && ops == 8)   // per register and map: one write, one read
        u.stuck = 1; u.r1 = 0;
        q = uvm_reg_access_seq::type_id::create("q");
        go(q, ops, errs);
        `CHK("E.3 reg_access_seq finds a stuck bit", errs > 0)
        u.stuck = 0; b.reset(); u.r0 = 0; u.r1 = 0;
        begin
          uvm_reg_single_access_seq one = uvm_reg_single_access_seq::type_id::create("one");
          one.rg = b.r0;
          go(one, ops, errs);
          `CHK("E.3 reg_single_access_seq", errs == 0 && ops == 4)
        end
      end
      begin   // E.4
        uvm_reg_shared_access_seq rs = uvm_reg_shared_access_seq::type_id::create("rs");
        uvm_mem_shared_access_seq ms = uvm_mem_shared_access_seq::type_id::create("ms");
        uvm_reg_mem_shared_access_seq all = uvm_reg_mem_shared_access_seq::type_id::create("all");
        rs.rg = b.r0;
        go(rs, ops, errs);
        `CHK("E.4 reg_shared_access_seq", errs == 0 && ops == 6)   // per map: one write, read through both
        ms.mem = b.m;
        go(ms, ops, errs);
        `CHK("E.4 mem_shared_access_seq", errs == 0 && ops == 8 * 2 * 4)   // per word and map: read, write, read through both
        go(all, ops, errs);
        `CHK("E.4 reg_mem_shared_access_seq", errs == 0 && ops == 2 * 6 + 64)
        u.stuck = 1; u.r1 = 0; b.reset();
        rs = uvm_reg_shared_access_seq::type_id::create("rs"); rs.rg = b.r1;
        go(rs, ops, errs);
        `CHK("E.4 finds a stuck bit", errs > 0)
        u.stuck = 0; u.r0 = 0; u.r1 = 0; b.reset();
      end
      begin   // E.5
        uvm_mem_access_seq q = uvm_mem_access_seq::type_id::create("q");
        go(q, ops, errs);
        `CHK("E.5 mem_access_seq", errs == 0 && ops == 8 * 2 * 2)   // per word and map: one write, one read
        u.memfault = 1;
        q = uvm_mem_access_seq::type_id::create("q");
        go(q, ops, errs);
        `CHK("E.5 mem_access_seq finds a bad cell", errs > 0)
        u.memfault = 0;
        begin
          uvm_mem_single_access_seq one = uvm_mem_single_access_seq::type_id::create("one");
          one.mem = b.m;
          go(one, ops, errs);
          `CHK("E.5 mem_single_access_seq", errs == 0 && ops == 32)
        end
      end
      begin   // E.7
        uvm_reg_mem_hdl_paths_seq q = uvm_reg_mem_hdl_paths_seq::type_id::create("q");
        go(q, ops, errs);
        `CHK("E.7 hdl_paths_seq", errs == 0 && ops == 0)
        b.r1.clear_hdl_path(); b.r1.add_hdl_path_slice("no_such_reg", 0, 32);
        q = uvm_reg_mem_hdl_paths_seq::type_id::create("q");
        go(q, ops, errs);
        `CHK("E.7 hdl_paths_seq finds a bad path", errs > 0)
        b.r1.clear_hdl_path(); b.r1.add_hdl_path_slice("r1", 0, 32);
        q = uvm_reg_mem_hdl_paths_seq::type_id::create("q");
        q.abstractions.push_back("RTL");
        go(q, ops, errs);
        `CHK("E.7 abstractions", errs == 0)
      end
      begin   // E.8
        uvm_reg_mem_built_in_seq q = uvm_reg_mem_built_in_seq::type_id::create("q");
        int all_ops;
        u.r0 = 0; u.r1 = 0; b.reset();
        go(q, all_ops, errs);
        `CHK("E.8 built_in_seq all", errs == 0 && all_ops > 300)
        u.r0 = 0; u.r1 = 0; b.reset();
        q = uvm_reg_mem_built_in_seq::type_id::create("q");
        q.tests = UVM_DO_REG_HW_RESET | UVM_DO_REG_ACCESS;
        go(q, ops, errs);
        `CHK("E.8 built_in_seq tests", errs == 0 && ops == 4 + 8)   // reset: one read per register and map
        uvm_resource_db #(bit)::set({"REG::", b.get_full_name(), ".*"}, "NO_REG_TESTS", 1, this);
        q = uvm_reg_mem_built_in_seq::type_id::create("q");
        go(q, ops, errs);
        `CHK("E.8 NO_REG_TESTS", errs == 0 && ops == 0)
      end
      phase.drop_objection(this);
    endtask
    function void report_phase(uvm_phase phase);
      uvm_report_server rs = uvm_report_server::get_server();
      `CHK("no UVM_ERROR", rs.get_severity_count(UVM_ERROR) == 0 && rs.get_severity_count(UVM_FATAL) == 0)
      if (fails == 0) $display("ALL OK"); else $display("%0d FAILED", fails);
    endfunction
  endclass
  initial run_test("test");
endmodule
//> ok E.3 reg_access_seq
//> ok E.3 reg_access_seq finds a stuck bit
//> ok E.3 reg_single_access_seq
//> ok E.4 reg_shared_access_seq
//> ok E.4 mem_shared_access_seq
//> ok E.4 reg_mem_shared_access_seq
//> ok E.4 finds a stuck bit
//> ok E.5 mem_access_seq
//> ok E.5 mem_access_seq finds a bad cell
//> ok E.5 mem_single_access_seq
//> ok E.7 hdl_paths_seq
//> ok E.7 hdl_paths_seq finds a bad path
//> ok E.7 abstractions
//> ok E.8 built_in_seq all
//> ok E.8 built_in_seq tests
//> ok E.8 NO_REG_TESTS
//> ok no UVM_ERROR
//> ALL OK
