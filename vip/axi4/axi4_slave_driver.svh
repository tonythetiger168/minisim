// SPDX-License-Identifier: Apache-2.0
// axi4_slave_driver -- reactive memory responder (no sequencer needed).
// Stores write data in cfg.mem (mem_vip_pkg::mem_model) and returns read data
// from it, on a bus of `AXI4_DATA_W bits. Supports:
//   * FIXED/INCR/WRAP, narrow and unaligned transfers, byte strobes (A3.4);
//   * several outstanding bursts per direction; with cfg.reorder, responses
//     to different IDs out of order, and with cfg.interleave, read data beats
//     of different IDs interleaved; same-ID order is always kept (A5, A6);
//   * an exclusive access monitor per ID (A7.2.3): an exclusive read sets it
//     and answers EXOKAY, any write to the monitored bytes clears it, and an
//     exclusive write answers EXOKAY and updates memory only if its monitor
//     still matches; else OKAY and memory is untouched. With
//     cfg.exclusive_support = 0 exclusive accesses are plain accesses
//     answered OKAY;
//   * error responses on address ranges (cfg.err_ranges; an erroring burst
//     does not touch memory);
//   * BUSER/RUSER echo AxUSER;
//   * random READY/VALID timing (cfg.max_*).
class axi4_slave_driver extends uvm_component;
  `uvm_component_utils(axi4_slave_driver)

  virtual axi4_if vif;
  axi4_cfg        cfg;

  mailbox #(axi4_item) aw_mb;
  protected axi4_item b_q[$];   // writes with all W beats in, waiting for B
  protected axi4_item r_q[$];   // reads accepted, R not finished

  // exclusive monitor, one per ID (A7.2.3)
  protected bit         ex_valid[axi4_id_t];
  protected axi4_item   ex_req[axi4_id_t];

  int unsigned n_writes, n_reads, n_excl_ok, n_excl_fail;

  function new(string name, uvm_component parent);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    if (!uvm_config_db#(virtual axi4_if)::get(this, "", "vif", vif))
      `uvm_fatal("NOVIF", {get_full_name(), ": axi4_if not set"})
    if (!uvm_config_db#(axi4_cfg)::get(this, "", "cfg", cfg))
      `uvm_fatal("NOCFG", {get_full_name(), ": axi4_cfg not set"})
    aw_mb = new();
  endfunction

  task run_phase(uvm_phase phase);
    vif.slv_cb.awready <= 1'b0; vif.slv_cb.wready <= 1'b0; vif.slv_cb.bvalid <= 1'b0; vif.slv_cb.arready <= 1'b0;
    vif.rvalid <= 1'b0; vif.rlast <= 1'b0; vif.slv_cb.bid <= '0; vif.slv_cb.bresp <= '0; vif.slv_cb.buser <= '0;
    vif.slv_cb.rid <= '0; vif.slv_cb.rdata <= '0; vif.slv_cb.rresp <= '0; vif.slv_cb.ruser <= '0;
    @(posedge vif.aclk iff vif.aresetn === 1'b1);
    fork
      aw_thread();
      w_thread();
      b_thread();
      ar_thread();
      r_thread();
    join
  endtask

  // ---------------------------------------------------------------- memory
  protected function axi4_data_t mem_read(axi4_addr_t a);
    axi4_addr_t base = a & ~(axi4_addr_t'(`AXI4_STRB_W) - 1);
    axi4_data_t d = '0;
    for (int i = 0; i < `AXI4_STRB_W; i++) d[8*i +: 8] = cfg.mem.read_byte(base + i);
    return d;
  endfunction

  protected function void mem_write(axi4_addr_t a, axi4_data_t d, axi4_strb_t s);
    axi4_addr_t base = a & ~(axi4_addr_t'(`AXI4_STRB_W) - 1);
    for (int i = 0; i < `AXI4_STRB_W; i++) if (s[i]) cfg.mem.write_byte(base + i, d[8*i +: 8]);
  endfunction

  // ---------------------------------------------------------------- exclusive monitor
  // same exclusive sequence (A7.2.4): ID, address and the burst attributes match
  protected function bit ex_match(axi4_item w);
    axi4_item r;
    if (!ex_valid.exists(w.id) || !ex_valid[w.id]) return 0;
    r = ex_req[w.id];
    return r.addr == w.addr && r.len == w.len && r.size == w.size && r.burst == w.burst &&
           r.cache == w.cache && r.prot == w.prot && r.region == w.region;
  endfunction

  // a write that updates memory clears every monitor on the bytes it covers
  protected function void ex_clear(axi4_item w);
    foreach (ex_valid[id]) if (ex_valid[id] && ex_req[id].overlaps(w)) ex_valid[id] = 0;
  endfunction

  protected function axi4_item capture(bit write);
    axi4_item t = axi4_item::type_id::create(write ? "aw" : "ar");
    t.write = write;
    if (write) begin
      t.id = vif.slv_cb.awid; t.addr = vif.slv_cb.awaddr; t.len = vif.slv_cb.awlen;
      t.size = vif.slv_cb.awsize; t.burst = axi4_burst_e'(vif.slv_cb.awburst);
      t.lock = vif.slv_cb.awlock; t.cache = vif.slv_cb.awcache; t.prot = vif.slv_cb.awprot;
      t.qos = vif.slv_cb.awqos; t.region = vif.slv_cb.awregion; t.auser = vif.slv_cb.awuser;
    end else begin
      t.id = vif.slv_cb.arid; t.addr = vif.slv_cb.araddr; t.len = vif.slv_cb.arlen;
      t.size = vif.slv_cb.arsize; t.burst = axi4_burst_e'(vif.slv_cb.arburst);
      t.lock = vif.slv_cb.arlock; t.cache = vif.slv_cb.arcache; t.prot = vif.slv_cb.arprot;
      t.qos = vif.slv_cb.arqos; t.region = vif.slv_cb.arregion; t.auser = vif.slv_cb.aruser;
    end
    t.alloc();
    return t;
  endfunction

  // ---------------------------------------------------------------- write path
  task aw_thread();
    forever begin
      repeat (axi4_cfg::rand_delay(cfg.max_aready_delay)) @(vif.slv_cb);
      vif.slv_cb.awready <= 1'b1;
      do @(vif.slv_cb); while (!(vif.slv_cb.awvalid === 1'b1 && vif.awready === 1'b1));
      vif.slv_cb.awready <= 1'b0;
      aw_mb.put(capture(1));
    end
  endtask

  task w_thread();
    axi4_item t;
    forever begin
      axi4_resp_e rsp;
      aw_mb.get(t);
      for (int unsigned i = 0; i < t.beats(); i++) begin
        int unsigned d = axi4_cfg::rand_delay(cfg.max_wready_delay);
        if (d) begin
          vif.slv_cb.wready <= 1'b0;
          repeat (d) @(vif.slv_cb);
        end
        vif.slv_cb.wready <= 1'b1;
        do @(vif.slv_cb); while (!(vif.slv_cb.wvalid === 1'b1 && vif.wready === 1'b1));
        t.data[i]  = vif.slv_cb.wdata;
        t.strb[i]  = vif.slv_cb.wstrb;
        t.wuser[i] = vif.slv_cb.wuser;
        if (vif.slv_cb.wlast !== (i == t.len))
          `uvm_error("AXI4_SDRV", $sformatf("WLAST=%0b on beat %0d of %0d (addr %0h)",
                     vif.slv_cb.wlast, i, t.beats(), t.addr))
        if ((t.strb[i] & ~t.lanes(i)) != 0)
          `uvm_error("AXI4_SDRV", $sformatf("WSTRB %b outside the lanes %b of beat %0d (addr %0h size %0d)",
                     t.strb[i], t.lanes(i), i, t.beat_addr(i), t.size))
      end
      vif.slv_cb.wready <= 1'b0;
      // response and memory update once the whole burst is in (A7.2.3)
      rsp = cfg.resp_for(t.addr);
      if (rsp == AXI4_OKAY) begin
        bit commit = 1;
        if (t.lock && cfg.exclusive_support) begin
          if (ex_match(t)) begin rsp = AXI4_EXOKAY; n_excl_ok++; end
          else begin commit = 0; n_excl_fail++; end
        end
        if (commit) begin
          for (int unsigned i = 0; i < t.beats(); i++) mem_write(t.beat_addr(i), t.data[i], t.strb[i]);
          ex_clear(t);
        end
      end
      t.resp[0] = rsp;
      t.buser = t.auser;
      n_writes++;
      b_q.push_back(t);
    end
  endtask

  // index into q of the burst to answer next: the oldest, or with reorder a
  // random one among the oldest burst of each ID
  protected function int pick(ref axi4_item q[$]);
    int heads[$];
    if (!cfg.reorder) return 0;
    foreach (q[i]) begin
      bit first = 1;
      for (int j = 0; j < i; j++) if (q[j].id == q[i].id) begin first = 0; break; end
      if (first) heads.push_back(i);
    end
    return heads[$urandom_range(heads.size() - 1, 0)];
  endfunction

  task b_thread();
    forever begin
      int k;
      axi4_item t;
      wait (b_q.size() > 0);
      // re-synchronize: a clocking drive issued right after a wakeup only
      // matures one edge later, which merged back-to-back B responses into
      // one handshake when the master held BREADY high
      @(vif.slv_cb);
      repeat (axi4_cfg::rand_delay(cfg.max_b_latency)) @(vif.slv_cb);
      k = pick(b_q);
      t = b_q[k];
      b_q.delete(k);
      vif.slv_cb.bid    <= t.id;
      vif.slv_cb.bresp  <= t.resp[0];
      vif.slv_cb.buser  <= t.buser;
      vif.slv_cb.bvalid <= 1'b1;
      do @(vif.slv_cb); while (!(vif.slv_cb.bready === 1'b1 && vif.bvalid === 1'b1));
      vif.slv_cb.bvalid <= 1'b0;
    end
  endtask

  // ---------------------------------------------------------------- read path
  task ar_thread();
    forever begin
      axi4_item t;
      repeat (axi4_cfg::rand_delay(cfg.max_aready_delay)) @(vif.slv_cb);
      vif.slv_cb.arready <= 1'b1;
      do @(vif.slv_cb); while (!(vif.slv_cb.arvalid === 1'b1 && vif.arready === 1'b1));
      vif.slv_cb.arready <= 1'b0;
      t = capture(0);
      t.n_rbeats = 0;
      begin
        axi4_resp_e rsp = cfg.resp_for(t.addr);
        if (rsp == AXI4_OKAY && t.lock && cfg.exclusive_support) begin
          // A7.2.3: record the address and ID; the read answers EXOKAY
          ex_valid[t.id] = 1;
          ex_req[t.id] = t;
          rsp = AXI4_EXOKAY;
        end
        foreach (t.resp[i]) t.resp[i] = rsp;
      end
      r_q.push_back(t);
    end
  endtask

  task r_thread();
    axi4_item cur;
    forever begin
      int k;
      axi4_item t;
      int unsigned i;
      if (r_q.size() == 0) begin
        vif.slv_cb.rvalid <= 1'b0;
        vif.slv_cb.rlast  <= 1'b0;
        wait (r_q.size() > 0);
        @(vif.slv_cb);                 // re-synchronize (see b_thread)
        repeat (axi4_cfg::rand_delay(cfg.max_r_latency)) @(vif.slv_cb);
      end else begin
        int unsigned g = axi4_cfg::rand_delay(cfg.max_rvalid_gap);
        if (g) begin
          vif.slv_cb.rvalid <= 1'b0;
          repeat (g) @(vif.slv_cb);
        end
      end
      // keep the burst in progress unless interleaving, else pick the next one
      k = -1;
      if (cur != null && !cfg.interleave)
        foreach (r_q[j]) if (r_q[j] == cur) begin k = j; break; end
      if (k < 0) k = pick(r_q);
      t = r_q[k];
      cur = t;
      i = t.n_rbeats;
      vif.slv_cb.rid    <= t.id;
      vif.slv_cb.rdata  <= (t.resp[i] inside {AXI4_OKAY, AXI4_EXOKAY}) ? mem_read(t.beat_addr(i)) : '0;
      vif.slv_cb.rresp  <= t.resp[i];
      vif.slv_cb.ruser  <= t.auser;
      vif.slv_cb.rlast  <= (i == t.len);
      vif.slv_cb.rvalid <= 1'b1;
      do @(vif.slv_cb); while (!(vif.slv_cb.rready === 1'b1 && vif.rvalid === 1'b1));
      t.n_rbeats = i + 1;
      if (i == t.len) begin
        foreach (r_q[j]) if (r_q[j] == t) begin r_q.delete(j); break; end
        cur = null;
        n_reads++;
      end
    end
  endtask

  function void report_phase(uvm_phase phase);
    `uvm_info("AXI4_SDRV", $sformatf("%s: served %0d writes, %0d reads, exclusive writes %0d ok %0d failed; %s",
              get_full_name(), n_writes, n_reads, n_excl_ok, n_excl_fail, cfg.mem.convert2string()), UVM_LOW)
  endfunction
endclass
