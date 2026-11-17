// SPDX-License-Identifier: Apache-2.0
// axi4_monitor -- passive: rebuilds complete bursts from the five channels and
// publishes each one on `ap` when it completes (B handshake / RLAST beat).
// Handles W data ahead of AW (A3.3), several outstanding bursts, responses
// out of order across IDs and read data interleaved across IDs (A5, A6);
// within an ID, responses go to the oldest burst. Transaction-level checks:
// beat counts and RLAST (A3.4.1), BID/RID of an outstanding burst (A3.3.1),
// EXOKAY only for exclusive accesses and no OKAY/EXOKAY mix in an exclusive
// read (A7.2.5).
class axi4_monitor extends uvm_monitor;
  `uvm_component_utils(axi4_monitor)

  virtual axi4_if vif;
  axi4_cfg        cfg;
  uvm_analysis_port #(axi4_item) ap;

  protected axi4_item   aw_q[$];        // AW seen, W burst not complete
  protected axi4_item   b_wait_q[$];    // AW + all W beats seen, waiting for B
  protected axi4_item   r_wait_q[$];    // AR seen, waiting for (more) R beats
  protected int unsigned r_beat[$];
  protected axi4_data_t w_data[$];      // beats of the W burst in progress
  protected axi4_strb_t w_strb[$];
  protected axi4_user_t w_user[$];
  protected axi4_data_t w_early_data[$][$];   // complete W bursts not yet paired with an AW
  protected axi4_strb_t w_early_strb[$][$];
  protected axi4_user_t w_early_user[$][$];

  int unsigned n_wr, n_rd;
  // traffic shape seen: responses out of order across IDs, read beats
  // interleaved between bursts, most reads / writes in flight at once
  int unsigned n_out_of_order, n_interleaved, max_rd_inflight, max_wr_inflight;
  protected axi4_item r_last_item;     // burst of the previous R beat, if unfinished

  function new(string name, uvm_component parent);
    super.new(name, parent);
    ap = new("ap", this);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    if (!uvm_config_db#(virtual axi4_if)::get(this, "", "vif", vif))
      `uvm_fatal("NOVIF", {get_full_name(), ": axi4_if not set"})
    if (!uvm_config_db#(axi4_cfg)::get(this, "", "cfg", cfg))
      `uvm_fatal("NOCFG", {get_full_name(), ": axi4_cfg not set"})
  endfunction

  protected function void flush_state();
    r_last_item = null;
    aw_q.delete(); b_wait_q.delete(); r_wait_q.delete(); r_beat.delete();
    w_data.delete(); w_strb.delete(); w_user.delete();
    w_early_data.delete(); w_early_strb.delete(); w_early_user.delete();
  endfunction

  // pair complete W bursts with their AW (W bursts arrive in AW order)
  protected function void pair_writes();
    while (aw_q.size() && w_early_data.size()) begin
      axi4_item t = aw_q.pop_front();
      axi4_data_t d[$] = w_early_data.pop_front();
      axi4_strb_t s[$] = w_early_strb.pop_front();
      axi4_user_t u[$] = w_early_user.pop_front();
      if (d.size() != t.beats())
        `uvm_error("AXI4_MON", $sformatf("%s: W burst has %0d beats, AWLEN says %0d (addr %0h)",
                   get_full_name(), d.size(), t.beats(), t.addr))
      foreach (d[i]) if (i < t.beats()) begin
        t.data[i]  = d[i];
        t.strb[i]  = s[i];
        t.wuser[i] = u[i];
      end
      b_wait_q.push_back(t);
    end
  endfunction

  protected function axi4_item capture(bit write);
    axi4_item t = axi4_item::type_id::create(write ? "mon_wr" : "mon_rd");
    t.write = write;
    if (write) begin
      t.id = vif.mon_cb.awid; t.addr = vif.mon_cb.awaddr; t.len = vif.mon_cb.awlen;
      t.size = vif.mon_cb.awsize; t.burst = axi4_burst_e'(vif.mon_cb.awburst);
      t.lock = vif.mon_cb.awlock; t.cache = vif.mon_cb.awcache; t.prot = vif.mon_cb.awprot;
      t.qos = vif.mon_cb.awqos; t.region = vif.mon_cb.awregion; t.auser = vif.mon_cb.awuser;
    end else begin
      t.id = vif.mon_cb.arid; t.addr = vif.mon_cb.araddr; t.len = vif.mon_cb.arlen;
      t.size = vif.mon_cb.arsize; t.burst = axi4_burst_e'(vif.mon_cb.arburst);
      t.lock = vif.mon_cb.arlock; t.cache = vif.mon_cb.arcache; t.prot = vif.mon_cb.arprot;
      t.qos = vif.mon_cb.arqos; t.region = vif.mon_cb.arregion; t.auser = vif.mon_cb.aruser;
    end
    t.port = cfg.port;
    t.t_start = $time;
    t.alloc();
    return t;
  endfunction

  task run_phase(uvm_phase phase);
    forever begin
      @(vif.mon_cb);
      if (vif.aresetn !== 1'b1) begin
        flush_state();
        continue;
      end
      // ---- AW ----
      if (vif.mon_cb.awvalid && vif.mon_cb.awready) begin
        aw_q.push_back(capture(1));
        if (aw_q.size() + b_wait_q.size() > max_wr_inflight) max_wr_inflight = aw_q.size() + b_wait_q.size();
      end
      // ---- W ----
      if (vif.mon_cb.wvalid && vif.mon_cb.wready) begin
        w_data.push_back(vif.mon_cb.wdata);
        w_strb.push_back(vif.mon_cb.wstrb);
        w_user.push_back(vif.mon_cb.wuser);
        if (vif.mon_cb.wlast) begin
          w_early_data.push_back(w_data);
          w_early_strb.push_back(w_strb);
          w_early_user.push_back(w_user);
          w_data.delete();
          w_strb.delete();
          w_user.delete();
        end
      end
      pair_writes();
      // ---- B ----
      if (vif.mon_cb.bvalid && vif.mon_cb.bready) begin
        int idx = -1;
        foreach (b_wait_q[i]) if (b_wait_q[i].id == vif.mon_cb.bid) begin idx = i; break; end
        if (idx < 0)
          `uvm_error("AXI4_MON", $sformatf("%s: B with ID %0h and no completed write burst",
                     get_full_name(), vif.mon_cb.bid))
        else begin
          axi4_item t = b_wait_q[idx];
          if (idx > 0) n_out_of_order++;
          b_wait_q.delete(idx);
          t.resp[0] = axi4_resp_e'(vif.mon_cb.bresp);
          t.buser = vif.mon_cb.buser;
          t.t_end = $time;
          if (t.resp[0] == AXI4_EXOKAY && !t.lock)
            `uvm_error("AXI4_MON", $sformatf("%s: EXOKAY for a non-exclusive write: %s", get_full_name(),
                       t.convert2string()))
          n_wr++;
          `uvm_info("AXI4_MON", {get_full_name(), ": ", t.convert2string()}, UVM_HIGH)
          ap.write(t);
        end
      end
      // ---- AR ----
      if (vif.mon_cb.arvalid && vif.mon_cb.arready) begin
        r_wait_q.push_back(capture(0));
        r_beat.push_back(0);
        if (r_wait_q.size() > max_rd_inflight) max_rd_inflight = r_wait_q.size();
      end
      // ---- R ----
      if (vif.mon_cb.rvalid && vif.mon_cb.rready) begin
        int idx = -1;
        foreach (r_wait_q[i]) if (r_wait_q[i].id == vif.mon_cb.rid) begin idx = i; break; end
        if (idx < 0)
          `uvm_error("AXI4_MON", $sformatf("%s: R with ID %0h and no outstanding read",
                     get_full_name(), vif.mon_cb.rid))
        else begin
          axi4_item t = r_wait_q[idx];
          int unsigned b = r_beat[idx];
          if (b == 0 && idx > 0) n_out_of_order++;
          if (r_last_item != null && r_last_item != t) n_interleaved++;
          r_last_item = t;
          if (b < t.beats()) begin
            t.data[b]  = vif.mon_cb.rdata;
            t.strb[b]  = t.lanes(b);
            t.resp[b]  = axi4_resp_e'(vif.mon_cb.rresp);
            t.ruser[b] = vif.mon_cb.ruser;
          end
          r_beat[idx] = b + 1;
          if (vif.mon_cb.rlast !== (b == t.len))
            `uvm_error("AXI4_MON", $sformatf("%s: RLAST=%0b on beat %0d of %0d (addr %0h)",
                       get_full_name(), vif.mon_cb.rlast, b, t.beats(), t.addr))
          if (vif.mon_cb.rlast || b >= t.len) begin
            bit ex = 0, ok = 0;
            r_wait_q.delete(idx);
            r_beat.delete(idx);
            r_last_item = null;
            t.t_end = $time;
            foreach (t.resp[i]) begin
              if (t.resp[i] == AXI4_EXOKAY) ex = 1;
              if (t.resp[i] == AXI4_OKAY) ok = 1;
            end
            if (ex && !t.lock)
              `uvm_error("AXI4_MON", $sformatf("%s: EXOKAY for a non-exclusive read: %s", get_full_name(),
                         t.convert2string()))
            if (ex && ok)
              `uvm_error("AXI4_MON", $sformatf("%s: exclusive read mixes OKAY and EXOKAY: %s", get_full_name(),
                         t.convert2string()))
            n_rd++;
            `uvm_info("AXI4_MON", {get_full_name(), ": ", t.convert2string()}, UVM_HIGH)
            ap.write(t);
          end
        end
      end
    end
  endtask

  function void check_phase(uvm_phase phase);
    if (aw_q.size() || b_wait_q.size() || r_wait_q.size() || w_data.size() || w_early_data.size())
      `uvm_error("AXI4_MON", $sformatf("%s: unfinished bursts at end of test (aw=%0d b=%0d r=%0d w_beats=%0d w_bursts=%0d)",
                 get_full_name(), aw_q.size(), b_wait_q.size(), r_wait_q.size(), w_data.size(),
                 w_early_data.size()))
  endfunction

  function void report_phase(uvm_phase phase);
    `uvm_info("AXI4_MON", $sformatf("%s: %0d writes, %0d reads; %0d responses out of order, %0d interleaved read beats, up to %0d writes and %0d reads in flight",
              get_full_name(), n_wr, n_rd, n_out_of_order, n_interleaved, max_wr_inflight, max_rd_inflight), UVM_LOW)
  endfunction
endclass
