// SPDX-License-Identifier: Apache-2.0
// axi4_master_driver -- issues bursts on the five channels, up to
// cfg.max_outstanding at once. AW and W run as separate threads in issue
// order (W may lead AW, A3.3; no write data interleaving in AXI4); responses
// are matched by ID to the oldest outstanding burst with that ID (A5.1), so
// out-of-order and interleaved read data across IDs are accepted.
// The response (read data, RRESP/BRESP, xUSER) is written back into the
// request item and item.done is set; with max_outstanding == 1 item_done is
// also delayed until then, so finish_item returns with the response.
class axi4_master_driver extends uvm_driver #(axi4_item);
  `uvm_component_utils(axi4_master_driver)

  virtual axi4_if vif;
  axi4_cfg        cfg;

  protected mailbox #(axi4_item) aw_mb, w_mb, ar_mb;
  protected axi4_item b_pend[$], r_pend[$];  // issued, waiting for the response
  protected semaphore slots;
  int unsigned n_writes, n_reads;

  function new(string name, uvm_component parent);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    if (!uvm_config_db#(virtual axi4_if)::get(this, "", "vif", vif))
      `uvm_fatal("NOVIF", {get_full_name(), ": axi4_if not set"})
    if (!uvm_config_db#(axi4_cfg)::get(this, "", "cfg", cfg))
      `uvm_fatal("NOCFG", {get_full_name(), ": axi4_cfg not set"})
    aw_mb = new(); w_mb = new(); ar_mb = new();
    slots = new(cfg.max_outstanding ? cfg.max_outstanding : 1);
  endfunction

  task reset_outputs();
    vif.mst_cb.awvalid <= 1'b0; vif.mst_cb.wvalid <= 1'b0; vif.mst_cb.arvalid <= 1'b0;
    vif.bready <= 1'b0; vif.mst_cb.rready <= 1'b0; vif.wlast <= 1'b0;
    vif.mst_cb.awid <= '0; vif.mst_cb.awaddr <= '0; vif.mst_cb.awlen <= '0; vif.mst_cb.awsize <= '0;
    vif.mst_cb.awburst <= '0; vif.mst_cb.awlock <= 1'b0; vif.mst_cb.awcache <= '0; vif.mst_cb.awprot <= '0;
    vif.mst_cb.awqos <= '0; vif.mst_cb.awregion <= '0; vif.mst_cb.awuser <= '0;
    vif.mst_cb.wdata <= '0; vif.mst_cb.wstrb <= '0; vif.mst_cb.wuser <= '0;
    vif.mst_cb.arid <= '0; vif.mst_cb.araddr <= '0; vif.mst_cb.arlen <= '0; vif.mst_cb.arsize <= '0;
    vif.mst_cb.arburst <= '0; vif.mst_cb.arlock <= 1'b0; vif.mst_cb.arcache <= '0; vif.mst_cb.arprot <= '0;
    vif.mst_cb.arqos <= '0; vif.mst_cb.arregion <= '0; vif.mst_cb.aruser <= '0;
  endtask

  task run_phase(uvm_phase phase);
    reset_outputs();
    @(posedge vif.aclk iff vif.aresetn === 1'b1);
    fork
      get_thread();
      aw_thread();
      w_thread();
      b_thread();
      ar_thread();
      r_thread();
    join
  endtask

  // fit the per-beat arrays to len (sequences may leave them short)
  protected function void fit(axi4_item t);
    if (t.data.size() != t.beats()) begin axi4_data_t d[] = t.data; t.data = new[t.beats()](d); end
    if (t.strb.size() != t.beats()) begin
      axi4_strb_t s[] = t.strb;
      int unsigned n = s.size();
      t.strb = new[t.beats()](s);
      for (int unsigned i = n; i < t.beats(); i++) t.strb[i] = t.lanes(i);
    end
    if (t.wuser.size() != t.beats()) begin axi4_user_t u[] = t.wuser; t.wuser = new[t.beats()](u); end
    t.resp  = new[t.write ? 1 : t.beats()];
    t.ruser = new[t.write ? 0 : t.beats()];
  endfunction

  task get_thread();
    forever begin
      axi4_item t;
      seq_item_port.get_next_item(req);
      t = req;
      fit(t);
      t.done = 0;
      t.n_rbeats = 0;
      slots.get(1);
      t.t_start = $time;
      if (t.write) begin
        b_pend.push_back(t);
        aw_mb.put(t);
        w_mb.put(t);
      end else begin
        r_pend.push_back(t);
        ar_mb.put(t);
      end
      if (cfg.max_outstanding <= 1) wait (t.done);
      seq_item_port.item_done();
    end
  endtask

  protected function void complete(axi4_item t);
    t.t_end = $time;
    t.done = 1;
    slots.put(1);
    if (t.write) n_writes++; else n_reads++;
    `uvm_info("AXI4_MDRV", {"done: ", t.convert2string()}, UVM_HIGH)
  endfunction

  task aw_thread();
    forever begin
      axi4_item t;
      aw_mb.get(t);
      @(vif.mst_cb);
      repeat (t.aw_delay) @(vif.mst_cb);
      vif.mst_cb.awid     <= t.id;
      vif.mst_cb.awaddr   <= t.addr;
      vif.mst_cb.awlen    <= t.len;
      vif.mst_cb.awsize   <= t.size;
      vif.mst_cb.awburst  <= t.burst;
      vif.mst_cb.awlock   <= t.lock;
      vif.mst_cb.awcache  <= t.cache;
      vif.mst_cb.awprot   <= t.prot;
      vif.mst_cb.awqos    <= t.qos;
      vif.mst_cb.awregion <= t.region;
      vif.mst_cb.awuser   <= t.auser;
      vif.mst_cb.awvalid  <= 1'b1;
      do @(vif.mst_cb); while (!(vif.mst_cb.awready === 1'b1 && vif.awvalid === 1'b1));
      vif.mst_cb.awvalid <= 1'b0;
    end
  endtask

  task w_thread();
    forever begin
      axi4_item t;
      w_mb.get(t);
      @(vif.mst_cb);
      for (int unsigned i = 0; i < t.beats(); i++) begin
        int unsigned gap = (i == 0) ? 0 : axi4_cfg::rand_delay(t.w_gap);
        if (gap) begin
          vif.mst_cb.wvalid <= 1'b0;
          repeat (gap) @(vif.mst_cb);
        end
        vif.mst_cb.wdata  <= t.data[i];
        vif.mst_cb.wstrb  <= t.strb[i];
        vif.mst_cb.wuser  <= t.wuser[i];
        vif.mst_cb.wlast  <= (i == t.len);
        vif.mst_cb.wvalid <= 1'b1;
        do @(vif.mst_cb); while (!(vif.mst_cb.wready === 1'b1 && vif.wvalid === 1'b1));
      end
      vif.mst_cb.wvalid <= 1'b0;
      vif.mst_cb.wlast  <= 1'b0;
    end
  endtask

  protected function int oldest(ref axi4_item q[$], input axi4_id_t id);
    foreach (q[i]) if (q[i].id == id) return i;
    return -1;
  endfunction

  task b_thread();
    forever begin
      int idx;
      wait (b_pend.size() > 0);
      @(vif.mst_cb);
      repeat (axi4_cfg::rand_delay(cfg.max_b_delay)) @(vif.mst_cb);
      vif.mst_cb.bready <= 1'b1;
      do @(vif.mst_cb); while (!(vif.mst_cb.bvalid === 1'b1 && vif.bready === 1'b1));
      vif.mst_cb.bready <= 1'b0;
      idx = oldest(b_pend, vif.mst_cb.bid);
      if (idx < 0)
        `uvm_error("AXI4_MDRV", $sformatf("B with BID %0h and no outstanding write with that ID", vif.mst_cb.bid))
      else begin
        axi4_item t = b_pend[idx];
        b_pend.delete(idx);
        t.resp[0] = axi4_resp_e'(vif.mst_cb.bresp);
        t.buser = vif.mst_cb.buser;
        complete(t);
      end
    end
  endtask

  task ar_thread();
    forever begin
      axi4_item t;
      ar_mb.get(t);
      @(vif.mst_cb);
      repeat (t.aw_delay) @(vif.mst_cb);
      vif.mst_cb.arid     <= t.id;
      vif.mst_cb.araddr   <= t.addr;
      vif.mst_cb.arlen    <= t.len;
      vif.mst_cb.arsize   <= t.size;
      vif.mst_cb.arburst  <= t.burst;
      vif.mst_cb.arlock   <= t.lock;
      vif.mst_cb.arcache  <= t.cache;
      vif.mst_cb.arprot   <= t.prot;
      vif.mst_cb.arqos    <= t.qos;
      vif.mst_cb.arregion <= t.region;
      vif.mst_cb.aruser   <= t.auser;
      vif.mst_cb.arvalid  <= 1'b1;
      do @(vif.mst_cb); while (!(vif.mst_cb.arready === 1'b1 && vif.arvalid === 1'b1));
      vif.mst_cb.arvalid <= 1'b0;
    end
  endtask

  task r_thread();
    forever begin
      int idx;
      int unsigned gap;
      wait (r_pend.size() > 0);
      gap = axi4_cfg::rand_delay(cfg.max_r_gap > r_pend[0].r_gap ? cfg.max_r_gap : r_pend[0].r_gap);
      if (gap) begin
        vif.mst_cb.rready <= 1'b0;
        repeat (gap) @(vif.mst_cb);
      end
      vif.mst_cb.rready <= 1'b1;
      do @(vif.mst_cb); while (!(vif.mst_cb.rvalid === 1'b1 && vif.rready === 1'b1));
      idx = oldest(r_pend, vif.mst_cb.rid);
      if (idx < 0)
        `uvm_error("AXI4_MDRV", $sformatf("R with RID %0h and no outstanding read with that ID", vif.mst_cb.rid))
      else begin
        axi4_item t = r_pend[idx];
        int unsigned i = t.n_rbeats;
        if (i < t.beats()) begin
          t.data[i]  = vif.mst_cb.rdata;
          t.strb[i]  = t.lanes(i);
          t.resp[i]  = axi4_resp_e'(vif.mst_cb.rresp);
          t.ruser[i] = vif.mst_cb.ruser;
        end
        if (vif.mst_cb.rlast !== (i == t.len))
          `uvm_error("AXI4_MDRV", $sformatf("RLAST=%0b on beat %0d of %0d (RID %0h)", vif.mst_cb.rlast, i,
                     t.beats(), t.id))
        t.n_rbeats = i + 1;
        if (vif.mst_cb.rlast === 1'b1 || i >= t.len) begin
          r_pend.delete(idx);
          complete(t);
        end
      end
      if (r_pend.size() == 0) vif.mst_cb.rready <= 1'b0;
    end
  endtask
endclass
