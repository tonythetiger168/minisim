// IEEE 1800.2-2020 register classes with no test of their own until now:
// uvm_reg_file (18.3), uvm_reg randomization (18.4), uvm_reg_indirect_data
// (18.7), uvm_reg_fifo (18.8), uvm_vreg and uvm_vreg_field (18.9, 18.10),
// uvm_mem_mam and its regions (18.12), and the uvm_reg_block lookups of
// Annex D.3. The bus driver reads and writes the DUT below, so front door
// and back door see the same storage.
//! args: +UVM_NO_RELNOTES
`include "uvm_macros.svh"
module rfm; reg [31:0] a = 32'h11, b = 32'h22; endmodule
module dut;
  reg [31:0] r0 = 0;
  reg [31:0] m [0:15];
  reg [7:0] idx = 0;
  reg [31:0] tbl [0:3];
  reg [31:0] fifo [$];
  rfm rf();
  int nwr, nrd;
  function automatic void wr(input bit [31:0] a, input bit [31:0] d);
    nwr++;
    case (a)
      'h00: r0 = d;
      'h08: rf.a = d;
      'h0c: rf.b = d;
      'h10: idx = d[7:0];
      'h14: tbl[idx[1:0]] = d;
      'h18: fifo.push_back(d);
      default: if (a >= 'h100 && a < 'h140) m[(a - 'h100) >> 2] = d;
    endcase
  endfunction
  function automatic bit [31:0] rd(input bit [31:0] a);
    nrd++;
    case (a)
      'h00: return r0;
      'h08: return rf.a;
      'h0c: return rf.b;
      'h10: return idx;
      'h14: return tbl[idx[1:0]];
      'h18: return fifo.size() ? fifo.pop_front() : 0;
      default: if (a >= 'h100 && a < 'h140) return m[(a - 'h100) >> 2];
    endcase
    return 0;
  endfunction
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
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    task run_phase(uvm_phase phase);
      forever begin
        seq_item_port.get_next_item(req); #2;
        if (req.wr) u.wr(req.addr, req.data); else req.data = u.rd(req.addr);
        seq_item_port.item_done();
      end
    endtask
  endclass

  class reg0 extends uvm_reg;
    rand uvm_reg_field lo, hi;
    constraint lo_small { lo.value < 16'h100; }
    `uvm_object_utils(reg0)
    function new(string name = "reg0"); super.new(name, 32, UVM_NO_COVERAGE); endfunction
    virtual function void build();
      lo = uvm_reg_field::type_id::create("lo"); lo.configure(this, 16, 0, "RW", 0, 16'h0, 1, 1, 1);
      hi = uvm_reg_field::type_id::create("hi"); hi.configure(this, 16, 16, "RW", 0, 16'h0, 1, 1, 1);
    endfunction
  endclass
  class reg32 extends uvm_reg;
    rand uvm_reg_field f;
    `uvm_object_utils(reg32)
    function new(string name = "reg32"); super.new(name, 32, UVM_NO_COVERAGE); endfunction
    virtual function void build();
      f = uvm_reg_field::type_id::create("f"); f.configure(this, 32, 0, "RW", 0, 32'h0, 1, 1, 1);
    endfunction
  endclass
  class reg8 extends uvm_reg;
    rand uvm_reg_field f;
    `uvm_object_utils(reg8)
    function new(string name = "reg8"); super.new(name, 8, UVM_NO_COVERAGE); endfunction
    virtual function void build();
      f = uvm_reg_field::type_id::create("f"); f.configure(this, 8, 0, "RW", 0, 8'h0, 1, 1, 1);
    endfunction
  endclass
  class idata_reg extends uvm_reg_indirect_data;
    `uvm_object_utils(idata_reg)
    function new(string name = "idata"); super.new(name, 32, UVM_NO_COVERAGE); endfunction
  endclass
  class fifo_reg extends uvm_reg_fifo;
    `uvm_object_utils(fifo_reg)
    function new(string name = "fifo"); super.new(name, 4, 32, UVM_NO_COVERAGE); endfunction
  endclass
  class vr_t extends uvm_vreg;
    uvm_vreg_field lo, hi;
    `uvm_object_utils(vr_t)
    function new(string name = "vr"); super.new(name, 32); endfunction
    virtual function void build();
      lo = uvm_vreg_field::type_id::create("lo"); lo.configure(this, 12, 0);
      hi = uvm_vreg_field::type_id::create("hi"); hi.configure(this, 20, 12);
    endfunction
  endclass
  class sub_blk extends uvm_reg_block;
    `uvm_object_utils(sub_blk)
    function new(string name = "sub"); super.new(name, UVM_NO_COVERAGE); endfunction
  endclass
  class blk extends uvm_reg_block;
    rand reg0 r0;
    uvm_reg_file rf; reg32 a, b;
    reg8 idx; reg32 tbl[4]; idata_reg idata;
    fifo_reg fifo;
    uvm_mem m; vr_t vr, dvr;
    sub_blk sub;
    `uvm_object_utils(blk)
    function new(string name = "blk"); super.new(name, UVM_NO_COVERAGE); endfunction
    virtual function void build();
      uvm_reg ta[] = new[4];
      default_map = create_map("map", 0, 4, UVM_LITTLE_ENDIAN);
      r0 = reg0::type_id::create("r0"); r0.configure(this, null, "r0"); r0.build();
      rf = uvm_reg_file::type_id::create("rf"); rf.configure(this, null, "rf");
      a = reg32::type_id::create("a"); a.configure(this, rf, "a"); a.build();
      b = reg32::type_id::create("b"); b.configure(this, rf, "b"); b.build();
      idx = reg8::type_id::create("idx"); idx.configure(this, null, "idx"); idx.build();
      foreach (tbl[i]) begin
        tbl[i] = reg32::type_id::create($sformatf("tbl%0d", i)); tbl[i].configure(this, null, $sformatf("tbl[%0d]", i)); tbl[i].build();
        ta[i] = tbl[i];
      end
      idata = idata_reg::type_id::create("idata"); idata.configure(idx, ta, this, null); idata.build();
      fifo = fifo_reg::type_id::create("fifo"); fifo.configure(this, null, ""); fifo.build();
      m = new("m", 16, 32, "RW", UVM_NO_COVERAGE); m.configure(this, "m");
      vr = vr_t::type_id::create("vr"); vr.configure(this, m, 4, 8, 1); vr.build();
      dvr = vr_t::type_id::create("dvr"); dvr.configure(this); dvr.build();
      sub = sub_blk::type_id::create("sub"); sub.configure(this, "");
      default_map.add_reg(r0, 'h0, "RW");
      default_map.add_reg(a, 'h8, "RW");
      default_map.add_reg(b, 'hc, "RW");
      default_map.add_reg(idx, 'h10, "RW");
      default_map.add_reg(idata, 'h14, "RW");
      default_map.add_reg(fifo, 'h18, "RW");
      default_map.add_mem(m, 'h100, "RW");
      add_hdl_path("top.u");
      lock_model();
    endfunction
  endclass

  class test extends uvm_test;
    `uvm_component_utils(test)
    blk b; drv d; uvm_sequencer #(bus_tx) s; adapter ad;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    function void build_phase(uvm_phase phase);
      b = blk::type_id::create("b"); b.build();
      d = drv::type_id::create("d", this); s = new("s", this); ad = adapter::type_id::create("ad");
    endfunction
    function void connect_phase(uvm_phase phase);
      d.seq_item_port.connect(s.seq_item_export);
      b.default_map.set_sequencer(s, ad);
      b.default_map.set_auto_predict(1);
    endfunction
    task run_phase(uvm_phase phase);
      uvm_status_e st; uvm_reg_data_t v; string paths[$]; int n0;
      phase.raise_objection(this);
      b.reset();
      // 18.3 uvm_reg_file
      `CHK("18.3 parent", b.rf.get_parent() == b && b.rf.get_block() == b && b.rf.get_regfile() == null)
      `CHK("18.3 names", b.rf.get_full_name() == "b.rf" && b.a.get_full_name() == "b.rf.a" && b.a.get_regfile() == b.rf)
      b.rf.get_full_hdl_path(paths);
      `CHK("18.3 hdl path", b.rf.has_hdl_path() && paths.size() == 1 && paths[0] == "top.u.rf")
      b.a.read(st, v, UVM_BACKDOOR);
      `CHK("18.3 backdoor read", st == UVM_IS_OK && v == 32'h11)
      b.b.write(st, 32'h5ca1ab1e);
      `CHK("18.3 frontdoor write", st == UVM_IS_OK && u.rf.b == 32'h5ca1ab1e)
      b.b.peek(st, v);
      `CHK("18.3 peek", v == 32'h5ca1ab1e)
      b.rf.clear_hdl_path();
      `CHK("18.3 clear_hdl_path", !b.rf.has_hdl_path())
      b.rf.add_hdl_path("rf");
      `CHK("18.3 add_hdl_path", b.rf.has_hdl_path("RTL") && b.rf.get_default_hdl_path() == "RTL")
      // 18.4 randomization: post_randomize copies value to the desired value
      if (!b.r0.randomize() with { lo.value == 16'h3; }) `uvm_error("R", "reg randomize")
      `CHK("18.4 randomize with", b.r0.lo.get() == 16'h3 && b.r0.needs_update())
      if (!b.randomize()) `uvm_error("R", "block randomize")
      `CHK("18.4 block randomize", b.r0.lo.get() < 16'h100 && b.r0.lo.get() == b.r0.lo.value && b.r0.hi.get() == b.r0.hi.value)
      b.r0.update(st);
      `CHK("18.4 update", u.r0 == b.r0.get() && !b.r0.needs_update())
      // 18.7 indirect data: idx selects the table entry behind idata
      n0 = u.nwr;
      b.tbl[2].write(st, 32'h2222);
      `CHK("18.7 table write", st == UVM_IS_OK && u.idx == 2 && u.tbl[2] == 32'h2222 && u.nwr == n0 + 2)
      b.tbl[1].write(st, 32'h1111);
      b.tbl[2].read(st, v);
      `CHK("18.7 table read", st == UVM_IS_OK && v == 32'h2222 && u.idx == 2)
      `CHK("18.7 mirror", b.tbl[1].get_mirrored_value() == 32'h1111 && b.tbl[2].get_mirrored_value() == 32'h2222)
      b.idx.write(st, 1);
      b.idata.read(st, v);
      `CHK("18.7 data read", v == 32'h1111)
      b.idata.write(st, 32'h1112);
      `CHK("18.7 data write", u.tbl[1] == 32'h1112 && b.tbl[1].get_mirrored_value() == 32'h1112)
      // 18.8 fifo
      `CHK("18.8 capacity", b.fifo.capacity() == 4 && b.fifo.size() == 0)
      b.fifo.set(32'ha1); b.fifo.set(32'ha2); b.fifo.set(32'ha3); b.fifo.set(32'ha4);
      b.fifo.set(32'ha5);   // full: ignored
      `CHK("18.8 set", b.fifo.size() == 4 && u.fifo.size() == 0 && b.fifo.get() == 32'ha1)
      b.fifo.update(st);
      `CHK("18.8 update", st == UVM_IS_OK && u.fifo.size() == 4 && u.fifo[0] == 32'ha1 && u.fifo[3] == 32'ha4 && b.fifo.size() == 4)
      b.fifo.read(st, v);
      `CHK("18.8 read", st == UVM_IS_OK && v == 32'ha1 && b.fifo.size() == 3 && u.fifo.size() == 3)
      b.fifo.mirror(st);
      `CHK("18.8 mirror", st == UVM_IS_OK && b.fifo.size() == 2 && u.fifo.size() == 2 && b.fifo.get() == 32'ha3)
      b.fifo.read(st, v);
      b.fifo.read(st, v);
      `CHK("18.8 drained", v == 32'ha4 && b.fifo.size() == 0 && u.fifo.size() == 0)
      n0 = u.nrd;
      b.fifo.read(st, v);   // empty: no bus access
      `CHK("18.8 read when empty", st == UVM_NOT_OK && u.nrd == n0)
      // 18.9, 18.10 virtual registers: vr[0..3] live in m[8..11]
      `CHK("18.9 static", b.vr.get_memory() == b.m && b.vr.get_size() == 4 && b.vr.get_n_memlocs() == 1 && b.vr.get_incr() == 1)
      `CHK("18.9 offset", b.vr.get_offset_in_memory(2) == 10 && b.vr.get_address(2) == 'h128)
      b.vr.write(2, st, 32'hdeadbeef);
      `CHK("18.9 write", st == UVM_IS_OK && u.m[10] == 32'hdeadbeef)
      u.m[9] = 32'h01234567;
      b.vr.read(1, st, v);
      `CHK("18.9 read", v == 32'h01234567)
      b.vr.peek(2, st, v);
      `CHK("18.9 peek", v == 32'hdeadbeef)
      b.vr.poke(3, st, 32'h33);
      `CHK("18.9 poke", u.m[11] == 32'h33)
      begin uvm_vreg_field fs[$]; b.vr.get_fields(fs); `CHK("18.9 fields", fs.size() == 2 && b.vr.get_field_by_name("hi") == b.vr.hi) end
      `CHK("18.10 field", b.vr.hi.get_parent() == b.vr && b.vr.hi.get_lsb_pos_in_register() == 12 && b.vr.hi.get_n_bits() == 20 &&
                          b.vr.lo.get_full_name() == "vr.lo" && b.vr.lo.get_access() == "RW")
      b.vr.lo.write(2, st, 12'h123);
      `CHK("18.10 field write", u.m[10] == 32'hdeadb123)
      b.vr.hi.read(2, st, v);
      `CHK("18.10 field read", v == 20'hdeadb)
      b.vr.hi.poke(1, st, 20'hfffff);
      b.vr.lo.peek(1, st, v);
      `CHK("18.10 field poke/peek", u.m[9] == 32'hfffff567 && v == 12'h567)
      // 18.12 memory allocation manager, and a virtual register placed by it
      begin
        uvm_mem_mam_cfg mc = new(); uvm_mem_mam mam; uvm_mem_region rg, r2, it; int n;
        mc.n_bytes = 4; mc.start_offset = 0; mc.end_offset = 7; mc.mode = uvm_mem_mam::GREEDY; mc.locality = uvm_mem_mam::BROAD;
        mam = new("mam", mc, b.m);
        `CHK("18.12 get_memory", mam.get_memory() == b.m)
        rg = mam.request_region(8);
        `CHK("18.12 request_region", rg != null && rg.get_len() == 2 && rg.get_n_bytes() == 8 &&
                                     rg.get_end_offset() == rg.get_start_offset() + 1 && rg.get_end_offset() <= 7 && rg.get_memory() == b.m)
        rg.write(st, 1, 32'h4d414d);
        `CHK("18.12 region write", st == UVM_IS_OK && u.m[rg.get_start_offset() + 1] == 32'h4d414d)
        rg.read(st, 1, v);
        `CHK("18.12 region read", v == 32'h4d414d)
        rg.poke(st, 0, 32'h77); rg.peek(st, 0, v);
        `CHK("18.12 region poke/peek", v == 32'h77 && u.m[rg.get_start_offset()] == 32'h77)
        mam.release_region(rg);
        r2 = mam.reserve_region(4, 16);
        `CHK("18.12 reserve_region", r2 != null && r2.get_start_offset() == 4 && r2.get_end_offset() == 7)
        rg = mam.request_region(16);
        `CHK("18.12 rest", rg != null && rg.get_start_offset() == 0 && rg.get_end_offset() == 3)
        for (it = mam.for_each(1); it != null; it = mam.for_each()) n++;
        `CHK("18.12 for_each", n == 2)
        mam.release_all_regions();
        `CHK("18.12 release_all", mam.for_each(1) == null)
        rg = b.dvr.allocate(2, mam);
        `CHK("18.9 allocate", rg != null && b.dvr.get_region() == rg && b.dvr.get_size() == 2 && rg.get_len() == 2)
          b.dvr.write(1, st, 32'hd1);
        `CHK("18.9 allocated write", u.m[rg.get_start_offset() + 1] == 32'hd1)
        b.dvr.release_region();
        `CHK("18.9 release_region", b.dvr.get_region() == null && mam.for_each(1) == null)
        `CHK("18.9 implement", b.dvr.implement(2, b.m, 14, 1) && b.dvr.get_offset_in_memory(1) == 15)
        b.dvr.write(1, st, 32'he1);
        `CHK("18.9 implemented write", u.m[15] == 32'he1)
      end
      // D.3 lookups
      begin
        uvm_reg_block bs[$]; uvm_vreg vs[$]; uvm_vreg_field vf;
        `CHK("D.3 get_reg_by_name", b.get_reg_by_name("r0") == b.r0 && b.get_reg_by_name("idx") == b.idx)
        `CHK("D.3 get_field_by_name", b.get_field_by_name("hi") == b.r0.hi)
        `CHK("D.3 get_mem_by_name", b.get_mem_by_name("m") == b.m)
        `CHK("D.3 get_vreg_by_name", b.get_vreg_by_name("vr") == b.vr)
        vf = b.get_vfield_by_name("lo");
        `CHK("D.3 get_vfield_by_name", vf != null && vf.get_name() == "lo")
        `CHK("D.3 get_block_by_name", b.get_block_by_name("sub") == b.sub && b.get_map_by_name("map") == b.default_map)
        b.get_blocks(bs);
        `CHK("D.3 get_blocks", bs.size() == 1 && bs[0] == b.sub)
        b.get_virtual_registers(vs);
        `CHK("D.3 get_virtual_registers", vs.size() == 2)
        bs.delete();
        `CHK("D.3 find_blocks", uvm_reg_block::find_blocks("b", bs) == 1 && bs[0] == b && uvm_reg_block::find_block("b.sub") == b.sub)
        bs.delete(); uvm_reg_block::get_root_blocks(bs);
        `CHK("D.3 get_root_blocks", bs.size() == 1 && bs[0] == b)
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
//> ok 18.3 parent
//> ok 18.3 names
//> ok 18.3 hdl path
//> ok 18.3 backdoor read
//> ok 18.3 frontdoor write
//> ok 18.3 peek
//> ok 18.3 clear_hdl_path
//> ok 18.3 add_hdl_path
//> ok 18.4 randomize with
//> ok 18.4 block randomize
//> ok 18.4 update
//> ok 18.7 table write
//> ok 18.7 table read
//> ok 18.7 mirror
//> ok 18.7 data read
//> ok 18.7 data write
//> ok 18.8 capacity
//> ok 18.8 set
//> ok 18.8 update
//> ok 18.8 read
//> ok 18.8 mirror
//> ok 18.8 drained
//> ok 18.8 read when empty
//> ok 18.9 static
//> ok 18.9 offset
//> ok 18.9 write
//> ok 18.9 read
//> ok 18.9 peek
//> ok 18.9 poke
//> ok 18.9 fields
//> ok 18.10 field
//> ok 18.10 field write
//> ok 18.10 field read
//> ok 18.10 field poke/peek
//> ok 18.12 get_memory
//> ok 18.12 request_region
//> ok 18.12 region write
//> ok 18.12 region read
//> ok 18.12 region poke/peek
//> ok 18.12 reserve_region
//> ok 18.12 rest
//> ok 18.12 for_each
//> ok 18.12 release_all
//> ok 18.9 allocate
//> ok 18.9 allocated write
//> ok 18.9 release_region
//> ok 18.9 implement
//> ok 18.9 implemented write
//> ok D.3 get_reg_by_name
//> ok D.3 get_field_by_name
//> ok D.3 get_mem_by_name
//> ok D.3 get_vreg_by_name
//> ok D.3 get_vfield_by_name
//> ok D.3 get_block_by_name
//> ok D.3 get_blocks
//> ok D.3 get_virtual_registers
//> ok D.3 find_blocks
//> ok D.3 get_root_blocks
//> ok no UVM_ERROR
//> ALL OK
