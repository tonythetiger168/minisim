// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// vip_test_pkg -- VIP self-tests: each master agent talks straight to the
// matching slave agent (no DUT), so the VIPs are checked against each other
// and against the memory model.
//   vip_axi4_loopback_test   random AXI4 bursts, master <-> memory responder
//   vip_axi4_outstanding_test  up to 8 bursts in flight, slave answers out of
//                            order across IDs and interleaves read data,
//                            random exclusive pairs
//   vip_axi4_excl_test       directed exclusive access cases (A7.2)
//   vip_apb_loopback_test    random APB transfers, requester <-> completer
// Pass: every read returns what an independent shadow model predicts, the
// monitors agree with the drivers, and the memory backdoor holds the data.
// ============================================================================
`ifndef VIP_TEST_PKG_SV
`define VIP_TEST_PKG_SV
package vip_test_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"
  import mem_vip_pkg::*;
  import axi4_pkg::*;
  import apb_pkg::*;

  `uvm_analysis_imp_decl(_axi_m)
  `uvm_analysis_imp_decl(_axi_s)
  `uvm_analysis_imp_decl(_apb_m)
  `uvm_analysis_imp_decl(_apb_s)

  // the 32-bit word that beat i of a size-2 burst carries, on any bus width
  function automatic bit [31:0] word(axi4_item t, int unsigned i);
    return t.data[i][8 * (t.beat_addr(i) % AXI4_STRB_WIDTH & ~3) +: 32];
  endfunction

  // shadow-memory checker for both loopbacks
  class vip_checker extends uvm_scoreboard;
    `uvm_component_utils(vip_checker)
    uvm_analysis_imp_axi_m #(axi4_item, vip_checker) axi_m_imp;
    uvm_analysis_imp_axi_s #(axi4_item, vip_checker) axi_s_imp;
    uvm_analysis_imp_apb_m #(apb_item, vip_checker)  apb_m_imp;
    uvm_analysis_imp_apb_s #(apb_item, vip_checker)  apb_s_imp;
    mem_model  axi_mem, apb_mem;
    axi4_cfg   axi_slv_cfg;
    apb_cfg    apb_slv_cfg;
    byte unsigned axi_shadow[axi4_addr_t], apb_shadow[bit [31:0]];
    axi4_item  axi_m_q[$], axi_s_q[$];
    apb_item   apb_m_q[$], apb_s_q[$];
    int unsigned n_checks, n_errors;

    function new(string name, uvm_component parent);
      super.new(name, parent);
      axi_m_imp = new("axi_m_imp", this);
      axi_s_imp = new("axi_s_imp", this);
      apb_m_imp = new("apb_m_imp", this);
      apb_s_imp = new("apb_s_imp", this);
    endfunction

    function void chk(bit ok, string what);
      n_checks++;
      if (!ok) begin n_errors++; `uvm_error("VIP_CHK", what) end
    endfunction

    function byte unsigned axi_exp(axi4_addr_t a);
      return axi_shadow.exists(a) ? axi_shadow[a] : mem_model::init_byte(axi_mem.init_kind, a);
    endfunction
    function byte unsigned apb_exp(bit [31:0] a);
      return apb_shadow.exists(a) ? apb_shadow[a] : mem_model::init_byte(apb_mem.init_kind, a);
    endfunction

    // the master-side monitor sees each AXI burst once: predict from it.
    // Exclusive accesses (A7.2.5): an exclusive read answers EXOKAY when the
    // slave supports them; an exclusive write answers EXOKAY (memory
    // updated) or OKAY (exclusive failed, memory untouched) -- which one is
    // checked by vip_axi4_excl_test, here the shadow follows the response.
    virtual function void write_axi_m(axi4_item t);
      axi4_resp_e exp = axi_slv_cfg.resp_for(t.addr);
      bit excl = t.lock && axi_slv_cfg.exclusive_support && exp == AXI4_OKAY;
      bit commit = exp == AXI4_OKAY;
      if (t.write && excl) commit = t.resp[0] == AXI4_EXOKAY;
      for (int unsigned i = 0; i < t.beats(); i++) begin
        axi4_addr_t base = t.beat_addr(i) & ~(axi4_addr_t'(AXI4_STRB_WIDTH) - 1);
        if (t.write) begin
          if (commit)
            for (int b = 0; b < AXI4_STRB_WIDTH; b++) if (t.strb[i][b]) axi_shadow[base + b] = t.data[i][8*b +: 8];
        end else begin
          axi4_resp_e e = excl ? AXI4_EXOKAY : exp;
          chk(t.resp[i] == e, $sformatf("AXI %s beat %0d resp %s, expected %s", t.convert2string(), i,
                t.resp[i].name(), e.name()));
          if (exp == AXI4_OKAY)
            for (int b = 0; b < AXI4_STRB_WIDTH; b++) if (t.lanes(i)[b])
              chk(t.data[i][8*b +: 8] == axi_exp(base + b),
                    $sformatf("AXI %s beat %0d byte %0d = %02h, expected %02h", t.convert2string(), i, b,
                              t.data[i][8*b +: 8], axi_exp(base + b)));
          chk(t.ruser[i] == t.auser, $sformatf("AXI %s beat %0d RUSER %0h, expected AxUSER echo %0h",
                t.convert2string(), i, t.ruser[i], t.auser));
        end
      end
      if (t.write) begin
        if (excl)
          chk(t.resp[0] inside {AXI4_OKAY, AXI4_EXOKAY}, $sformatf("AXI %s exclusive BRESP %s", t.convert2string(),
                t.resp[0].name()));
        else
          chk(t.resp[0] == exp, $sformatf("AXI %s BRESP, expected %s", t.convert2string(), exp.name()));
        chk(t.buser == t.auser, $sformatf("AXI %s BUSER %0h, expected AxUSER echo %0h", t.convert2string(),
              t.buser, t.auser));
      end
      axi_m_q.push_back(t);
      match_axi();
    endfunction

    virtual function void write_axi_s(axi4_item t);
      axi_s_q.push_back(t);
      match_axi();
    endfunction

    // both monitors watch the same wires: they must report identical bursts
    function void match_axi();
      while (axi_m_q.size() && axi_s_q.size()) begin
        axi4_item m = axi_m_q.pop_front(), s = axi_s_q.pop_front();
        bit ok = m.write == s.write && m.addr == s.addr && m.len == s.len && m.id == s.id &&
                 m.data == s.data && m.resp == s.resp && m.lock == s.lock && m.cache == s.cache &&
                 m.prot == s.prot && m.qos == s.qos && m.region == s.region && m.auser == s.auser &&
                 m.wuser == s.wuser && m.buser == s.buser && m.ruser == s.ruser;
        chk(ok, $sformatf("AXI monitors disagree: {%s} vs {%s}", m.convert2string(), s.convert2string()));
      end
    endfunction

    virtual function void write_apb_m(apb_item t);
      bit [31:0] base = t.addr & ~32'h3;
      bit err = apb_slv_cfg.is_err(t.addr);
      chk(t.slverr == err, $sformatf("APB %s: PSLVERR expected %0b", t.convert2string(), err));
      if (t.write && !err)
        for (int b = 0; b < 4; b++) if (t.strb[b]) apb_shadow[base + b] = t.data[8*b +: 8];
      if (!t.write && !err)
        for (int b = 0; b < 4; b++)
          chk(t.data[8*b +: 8] == apb_exp(base + b), $sformatf("APB %s: byte %0d expected %02h",
                t.convert2string(), b, apb_exp(base + b)));
      apb_m_q.push_back(t);
      while (apb_m_q.size() && apb_s_q.size()) begin
        apb_item a = apb_m_q.pop_front(), c = apb_s_q.pop_front();
        chk(a.addr == c.addr && a.data == c.data && a.write == c.write && a.slverr == c.slverr,
              $sformatf("APB monitors disagree: {%s} vs {%s}", a.convert2string(), c.convert2string()));
      end
    endfunction

    virtual function void write_apb_s(apb_item t);
      apb_s_q.push_back(t);
    endfunction

    function void check_phase(uvm_phase phase);
      // backdoor: the memory model must hold exactly what the shadow holds
      foreach (axi_shadow[a]) chk(axi_mem.read_byte(a) == axi_shadow[a],
                                    $sformatf("AXI memory backdoor at %0h", a));
      foreach (apb_shadow[a]) chk(apb_mem.read_byte(a) == apb_shadow[a],
                                    $sformatf("APB memory backdoor at %08h", a));
      if (n_checks == 0) `uvm_error("VIP_CHK", "no checks were made")
    endfunction

    function void report_phase(uvm_phase phase);
      `uvm_info("VIP_CHK", $sformatf("checks=%0d errors=%0d", n_checks, n_errors), UVM_NONE)
      if (n_errors == 0 && n_checks > 0) $display("SB_PASS checks=%0d", n_checks);
    endfunction
  endclass

  // ==========================================================================
  class vip_base_test extends uvm_test;
    `uvm_component_utils(vip_base_test)
    axi4_cfg    axi_m_cfg, axi_s_cfg;
    apb_cfg     apb_m_cfg, apb_s_cfg;
    axi4_agent  axi_m, axi_s;
    apb_agent   apb_m, apb_s;
    vip_checker chk;
    int unsigned n_items = 200;

    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction

    function void build_phase(uvm_phase phase);
      int v;
      super.build_phase(phase);
      if ($value$plusargs("SOC_ITEMS=%d", v)) n_items = v;
      axi_m_cfg = axi4_cfg::type_id::create("axi_m_cfg");
      axi_m_cfg.max_b_delay = 3; axi_m_cfg.max_r_gap = 3;
      axi_s_cfg = axi4_cfg::type_id::create("axi_s_cfg");
      axi_s_cfg.role = AXI4_SLAVE; axi_s_cfg.port = 1;
      axi_s_cfg.max_aready_delay = 3; axi_s_cfg.max_wready_delay = 3; axi_s_cfg.max_b_latency = 4;
      axi_s_cfg.max_r_latency = 6; axi_s_cfg.max_rvalid_gap = 3;
      axi_s_cfg.mem = mem_model::type_id::create("axi_mem");
      axi_s_cfg.add_err_range(32'h0000_F000, 32'h0000_FFFF, AXI4_SLVERR);
      axi_s_cfg.add_err_range(32'h0001_0000, 32'h0001_0FFF, AXI4_DECERR);
      apb_m_cfg = apb_cfg::type_id::create("apb_m_cfg");
      apb_s_cfg = apb_cfg::type_id::create("apb_s_cfg");
      apb_s_cfg.role = APB_SLAVE; apb_s_cfg.max_wait = 4;
      apb_s_cfg.mem = mem_model::type_id::create("apb_mem");
      apb_s_cfg.mem.init_kind = MEM_INIT_ZERO;
      apb_s_cfg.err_lo = 32'h0000_0F00; apb_s_cfg.err_hi = 32'h0000_0FFF;
      uvm_config_db#(axi4_cfg)::set(this, "axi_m", "cfg", axi_m_cfg);
      uvm_config_db#(axi4_cfg)::set(this, "axi_s", "cfg", axi_s_cfg);
      uvm_config_db#(apb_cfg)::set(this, "apb_m", "cfg", apb_m_cfg);
      uvm_config_db#(apb_cfg)::set(this, "apb_s", "cfg", apb_s_cfg);
      axi_m = axi4_agent::type_id::create("axi_m", this);
      axi_s = axi4_agent::type_id::create("axi_s", this);
      apb_m = apb_agent::type_id::create("apb_m", this);
      apb_s = apb_agent::type_id::create("apb_s", this);
      chk   = vip_checker::type_id::create("chk", this);
    endfunction

    function void connect_phase(uvm_phase phase);
      axi_m.ap.connect(chk.axi_m_imp);
      axi_s.ap.connect(chk.axi_s_imp);
      apb_m.ap.connect(chk.apb_m_imp);
      apb_s.ap.connect(chk.apb_s_imp);
      chk.axi_mem = axi_s_cfg.mem; chk.axi_slv_cfg = axi_s_cfg;
      chk.apb_mem = apb_s_cfg.mem; chk.apb_slv_cfg = apb_s_cfg;
    endfunction

    virtual task main();
    endtask

    task run_phase(uvm_phase phase);
      phase.raise_objection(this);
      main();
      #100ns;
      phase.drop_objection(this);
    endtask
  endclass

  class vip_axi4_loopback_test extends vip_base_test;
    `uvm_component_utils(vip_axi4_loopback_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    task main();
      axi4_random_seq s = axi4_random_seq::type_id::create("s");
      s.n_items = n_items; s.lo = 32'h0000_0000; s.hi = 32'h0001_0FFF; s.max_beats = 32;
      s.id_width = 8; s.max_delay = 3;
      s.start(axi_m.sqr);
      // directed: WRAP read back of a known pattern
      begin
        axi4_item w = axi4_api::make(1, 32'h100, 4, 2, AXI4_INCR), r;
        axi4_api::run(axi_m.sqr, w);
        r = axi4_api::make(0, 32'h108, 4, 2, AXI4_WRAP);
        axi4_api::run(axi_m.sqr, r);
        if (word(r, 0) != word(w, 2) || word(r, 1) != word(w, 3) || word(r, 2) != word(w, 0) || word(r, 3) != word(w, 1))
          `uvm_error("TEST", "WRAP read did not wrap at the 16-byte boundary")
      end
    endtask
  endclass

  // several bursts in flight; the slave reorders responses across IDs and
  // interleaves read data; random exclusive pairs among the traffic
  class vip_axi4_outstanding_test extends vip_base_test;
    `uvm_component_utils(vip_axi4_outstanding_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    function void build_phase(uvm_phase phase);
      super.build_phase(phase);
      axi_m_cfg.max_outstanding = 8;
      axi_s_cfg.reorder = 1;
      axi_s_cfg.interleave = 1;
    endfunction
    task main();
      axi4_random_seq s = axi4_random_seq::type_id::create("s");
      s.n_items = n_items; s.lo = 0; s.hi = 'h1_0FFF; s.max_beats = 16;
      s.id_width = 3; s.max_delay = 2; s.excl_pct = 5;
      s.start(axi_m.sqr);
      // the traffic must actually have had the shape this test is about
      chk.chk(axi_s.sdrv.n_excl_ok > 0, "no exclusive write succeeded");
      chk.chk(axi_m.mon.n_out_of_order > 0, "no response came back out of order");
      chk.chk(axi_m.mon.n_interleaved > 0, "no read data was interleaved");
      chk.chk(axi_m.mon.max_rd_inflight > 1 && axi_m.mon.max_wr_inflight > 1, "never more than one burst in flight");
    endtask
  endclass

  // directed exclusive access cases (A7.2): success, failure after another
  // write, monitor moved by a second exclusive read of the same ID, write
  // without a read, and a slave without exclusive support
  class vip_axi4_excl_test extends vip_base_test;
    `uvm_component_utils(vip_axi4_excl_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction

    task rd_back(axi4_addr_t a, int unsigned beats, output axi4_item r);
      r = axi4_api::make(0, a, beats, 2, AXI4_INCR, 0);
      axi4_api::run(axi_m.sqr, r);
    endtask

    function bit same_data(axi4_item a, axi4_item b);
      for (int unsigned i = 0; i < a.beats(); i++)
        if ((a.data[i] & strb_mask(a.lanes(i))) != (b.data[i] & strb_mask(b.lanes(i)))) return 0;
      return 1;
    endfunction

    static function axi4_data_t strb_mask(axi4_strb_t s);
      axi4_data_t m = '0;
      for (int b = 0; b < AXI4_STRB_WIDTH; b++) if (s[b]) m[8*b +: 8] = 8'hFF;
      return m;
    endfunction

    task main();
      axi4_item rd, wr, nw, back;
      bit ok;
      // 1. exclusive read + exclusive write, nothing in between: EXOKAY
      axi4_api::excl_read(axi_m.sqr, 'h200, 4, 2, 1, rd);
      chk.chk(rd.excl_ok(), {"exclusive read not EXOKAY: ", rd.convert2string()});
      axi4_api::excl_write(axi_m.sqr, rd, ok, wr);
      chk.chk(ok, "exclusive write with nothing in between failed");
      rd_back('h200, 4, back);
      chk.chk(same_data(back, wr), "successful exclusive write did not update memory");
      // 2. another write to the location in between: OKAY, memory not updated
      axi4_api::excl_read(axi_m.sqr, 'h300, 4, 2, 2, rd);
      nw = axi4_api::make(1, 'h304, 1, 2, AXI4_INCR, 3);
      axi4_api::run(axi_m.sqr, nw);
      axi4_api::excl_write(axi_m.sqr, rd, ok, wr);
      chk.chk(!ok, "exclusive write after an intervening write succeeded");
      rd_back('h300, 4, back);
      chk.chk(word(back, 1) == word(nw, 0), "failed exclusive write changed memory");
      // 3. a second exclusive read with the same ID moves the monitor (A7.2.1)
      axi4_api::excl_read(axi_m.sqr, 'h400, 2, 2, 4, rd);
      axi4_api::excl_read(axi_m.sqr, 'h500, 2, 2, 4, back);
      axi4_api::excl_write(axi_m.sqr, rd, ok, wr);
      chk.chk(!ok, "exclusive write to a no longer monitored address succeeded");
      axi4_api::excl_write(axi_m.sqr, back, ok, wr);
      chk.chk(ok, "exclusive write to the newly monitored address failed");
      // 4. an exclusive write with no exclusive read before it fails
      rd = axi4_api::make(0, 'h600, 1, 2, AXI4_INCR, 5);
      axi4_api::excl_write(axi_m.sqr, rd, ok, wr);
      chk.chk(!ok, "exclusive write without an exclusive read succeeded");
      // 5. a write by another ID to a different address leaves the monitor alone
      axi4_api::excl_read(axi_m.sqr, 'h700, 1, 2, 6, rd);
      nw = axi4_api::make(1, 'h708, 1, 2, AXI4_INCR, 7);
      axi4_api::run(axi_m.sqr, nw);
      axi4_api::excl_write(axi_m.sqr, rd, ok, wr);
      chk.chk(ok, "a write to another address cleared the exclusive monitor");
      // 6. slave without exclusive support (A7.2.3): OKAY, writes update memory
      axi_s_cfg.exclusive_support = 0;
      axi4_api::excl_read(axi_m.sqr, 'h800, 1, 2, 1, rd);
      chk.chk(rd.resp[0] == AXI4_OKAY, "exclusive read of a slave without support not OKAY");
      axi4_api::excl_write(axi_m.sqr, rd, ok, wr);
      chk.chk(!ok && wr.resp[0] == AXI4_OKAY, "exclusive write to a slave without support not OKAY");
      rd_back('h800, 1, back);
      chk.chk(same_data(back, wr), "exclusive write to a slave without support did not update memory");
      axi_s_cfg.exclusive_support = 1;
    endtask
  endclass

  class vip_apb_loopback_test extends vip_base_test;
    `uvm_component_utils(vip_apb_loopback_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    task main();
      apb_random_seq s = apb_random_seq::type_id::create("s");
      bit [31:0] d;
      bit err;
      s.n_items = n_items; s.lo = 0; s.hi = 32'hFFF;
      s.start(apb_m.sqr);
      apb_api::write(apb_m.sqr, 32'h40, 32'h1122_3344, err);
      apb_api::write(apb_m.sqr, 32'h40, 32'hAA00_0000, err, 4'b1000);
      apb_api::read(apb_m.sqr, 32'h40, d, err);
      if (d != 32'hAA22_3344 || err) `uvm_error("TEST", $sformatf("strobed write read back %08h", d))
    endtask
  endclass
endpackage
`endif
