// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// axi4_sva -- AXI4 protocol checker (bind or instantiate per port)
// ----------------------------------------------------------------------------
// Checks the AXI4 rules of AMBA AXI and ACE Protocol Specification
// IHI0022H.c. Every failure is a UVM error whose message starts with
// "[<section> <RULE>]", e.g. "[A3.4.1 AW_WRAP_LEN]"; docs/VIP.md lists the
// rules and spec/README.md maps them to the specification.
//
//   reset       A3.1.2  VALIDs LOW in reset and on the first edge after it
//   handshake   A3.2.1  VALID held until READY; payload stable meanwhile;
//                       VALID/READY never X; payload never X while VALID
//   bursts      A3.4.1  no reserved AxBURST; WRAP length 2/4/8/16 and aligned
//                       start; FIXED/WRAP at most 16 beats; AxSIZE within the
//                       bus; INCR does not cross 4 KB
//   attributes  A4.4    AxCACHE not a reserved encoding (Table A4-5)
//   exclusive   A7.2.4  size a power of two up to 128 bytes, <= 16 beats,
//                       address aligned to the total size
//               A7.2.5  EXOKAY only for exclusive accesses; an exclusive read
//                       never mixes OKAY and EXOKAY
//   write data  A3.4.4  WSTRB only on the lanes of the beat; WDATA not X on
//                       strobed lanes; A3.4.1 WLAST on the last beat only
//   ordering    A3.3.1  BVALID only after the AW and the last W handshake of a
//                       write with that BID; RVALID only for an outstanding
//                       read with that RID; RLAST on the last beat only;
//                       RDATA not X on the lanes of the beat (A3.4.4)
//   wait        (recommendation) VALID waiting more than MAX_WAIT cycles for
//                       READY is a warning; MAX_WAIT = 0 turns it off
//
// W data may lead its AW (A3.3): strobes and WLAST of early beats are checked
// when the AW arrives.
// ============================================================================
`ifndef AXI4_SVA_SV
`define AXI4_SVA_SV
`include "axi4_defines.svh"
module axi4_sva #(parameter string NAME = "axi4", parameter int MAX_WAIT = 0) (axi4_if vif);
  import uvm_pkg::*;
  `include "uvm_macros.svh"

  localparam int AW = `AXI4_ADDR_W;
  localparam int DW = `AXI4_DATA_W;
  localparam int SW = `AXI4_DATA_W / 8;
  localparam int IW = `AXI4_ID_W;

  wire clk = vif.aclk;
  wire rst_n = vif.aresetn;

  function automatic void err(string rule, string msg);
    `uvm_error({NAME, "_SVA"}, {"[", rule, "] ", msg})
  endfunction

  function automatic void warn(string rule, string msg);
    `uvm_warning({NAME, "_SVA"}, {"[", rule, "] ", msg})
  endfunction

  // ---------------------------------------------------------------- helpers
  function automatic bit crosses_4k(logic [AW-1:0] a, logic [7:0] len, logic [2:0] size, logic [1:0] burst);
    logic [AW-1:0] last;
    if (burst != 2'b01) return 0;
    last = (a & ~((AW'(1) << size) - 1)) + (AW'(len) << size);
    return a[AW-1:12] != last[AW-1:12];
  endfunction

  // A7.2.4: total bytes a power of two up to 128, at most 16 beats, aligned
  function automatic bit excl_ok(logic [AW-1:0] a, logic [7:0] len, logic [2:0] size);
    int unsigned bytes = (int'(len) + 1) << size;
    if (len > 15 || bytes > 128 || (bytes & (bytes - 1)) != 0) return 0;
    return (a & AW'(bytes - 1)) == 0;
  endfunction

  // A4.4 Table A4-5: with AxCACHE[1] (Modifiable) LOW, AxCACHE[3:2] are LOW
  function automatic bit cache_ok(logic [3:0] c);
    return c[1] || c[3:2] == 2'b00;
  endfunction

  // A3.4.1 beat address and A3.4.4 byte lanes of beat i
  function automatic logic [AW-1:0] beat_addr(logic [AW-1:0] a, logic [7:0] len, logic [2:0] size,
                                               logic [1:0] burst, int unsigned i);
    logic [AW-1:0] nb = AW'(1) << size;
    logic [AW-1:0] al = a & ~(nb - 1);
    logic [AW-1:0] ws, wb;
    case (burst)
      2'b00: return a;
      2'b10: begin
        ws = nb * (AW'(len) + 1);
        wb = a & ~(ws - 1);
        return wb + ((al - wb + AW'(i) * nb) % ws);
      end
      default: return (i == 0) ? a : al + AW'(i) * nb;
    endcase
  endfunction

  function automatic logic [SW-1:0] lanes(logic [AW-1:0] a, logic [2:0] size);
    int unsigned nb = 1 << size;
    int unsigned lo = a % SW;
    int unsigned hi = (a & ~(AW'(nb) - 1)) % SW + nb - 1;
    logic [SW-1:0] m = '0;
    for (int unsigned b = lo; b <= hi && b < SW; b++) m[b] = 1'b1;
    return m;
  endfunction

  function automatic bit lanes_x(logic [DW-1:0] d, logic [SW-1:0] m);
    for (int b = 0; b < SW; b++) if (m[b] && $isunknown(d[8*b +: 8])) return 1;
    return 0;
  endfunction

  // ------------------------------------------------- A3.1.2 reset behaviour
  reset_valids: assert property (@(posedge clk) !rst_n |-> !(vif.awvalid === 1'b1 || vif.wvalid === 1'b1 ||
                                  vif.arvalid === 1'b1 || vif.bvalid === 1'b1 || vif.rvalid === 1'b1))
    else err("A3.1.2 VALID_IN_RESET", "a VALID is HIGH while ARESETn is LOW");
  reset_exit: assert property (@(posedge clk) $rose(rst_n) |-> !(vif.awvalid === 1'b1 || vif.wvalid === 1'b1 ||
                                vif.arvalid === 1'b1))
    else err("A3.1.2 VALID_AT_RESET_EXIT", "AWVALID, WVALID or ARVALID HIGH on the first edge after reset");

  // ------------------------------------------------- A3.2.1 handshakes
  // AW
  aw_hold: assert property (@(posedge clk) disable iff (!rst_n) (vif.awvalid && !vif.awready) |=> vif.awvalid)
    else err("A3.2.1 AW_VALID_STABLE", "AWVALID dropped before AWREADY");
  aw_stable: assert property (@(posedge clk) disable iff (!rst_n) (vif.awvalid && !vif.awready) |=>
                 $stable({vif.awid, vif.awaddr, vif.awlen, vif.awsize, vif.awburst, vif.awlock, vif.awcache,
                          vif.awprot, vif.awqos, vif.awregion, vif.awuser}))
    else err("A3.2.1 AW_STABLE", "AW payload changed while AWVALID && !AWREADY");
  aw_vx: assert property (@(posedge clk) disable iff (!rst_n) !$isunknown({vif.awvalid, vif.awready}))
    else err("A3.2.1 AW_HS_X", "AWVALID or AWREADY is X/Z");
  aw_px: assert property (@(posedge clk) disable iff (!rst_n) vif.awvalid |->
                 !$isunknown({vif.awid, vif.awaddr, vif.awlen, vif.awsize, vif.awburst, vif.awlock, vif.awcache,
                              vif.awprot, vif.awqos, vif.awregion, vif.awuser}))
    else err("A3.2.1 AW_X", "AW payload has X/Z while AWVALID");
  // W
  w_hold: assert property (@(posedge clk) disable iff (!rst_n) (vif.wvalid && !vif.wready) |=> vif.wvalid)
    else err("A3.2.1 W_VALID_STABLE", "WVALID dropped before WREADY");
  w_stable: assert property (@(posedge clk) disable iff (!rst_n) (vif.wvalid && !vif.wready) |=>
                 $stable({vif.wdata, vif.wstrb, vif.wlast, vif.wuser}))
    else err("A3.2.1 W_STABLE", "W payload changed while WVALID && !WREADY");
  w_vx: assert property (@(posedge clk) disable iff (!rst_n) !$isunknown({vif.wvalid, vif.wready}))
    else err("A3.2.1 W_HS_X", "WVALID or WREADY is X/Z");
  w_px: assert property (@(posedge clk) disable iff (!rst_n) vif.wvalid |-> !$isunknown({vif.wstrb, vif.wlast, vif.wuser}))
    else err("A3.2.1 W_X", "WSTRB, WLAST or WUSER has X/Z while WVALID");
  // B
  b_hold: assert property (@(posedge clk) disable iff (!rst_n) (vif.bvalid && !vif.bready) |=> vif.bvalid)
    else err("A3.2.1 B_VALID_STABLE", "BVALID dropped before BREADY");
  b_stable: assert property (@(posedge clk) disable iff (!rst_n) (vif.bvalid && !vif.bready) |=>
                 $stable({vif.bid, vif.bresp, vif.buser}))
    else err("A3.2.1 B_STABLE", "B payload changed while BVALID && !BREADY");
  b_vx: assert property (@(posedge clk) disable iff (!rst_n) !$isunknown({vif.bvalid, vif.bready}))
    else err("A3.2.1 B_HS_X", "BVALID or BREADY is X/Z");
  b_px: assert property (@(posedge clk) disable iff (!rst_n) vif.bvalid |-> !$isunknown({vif.bid, vif.bresp, vif.buser}))
    else err("A3.2.1 B_X", "B payload has X/Z while BVALID");
  // AR
  ar_hold: assert property (@(posedge clk) disable iff (!rst_n) (vif.arvalid && !vif.arready) |=> vif.arvalid)
    else err("A3.2.1 AR_VALID_STABLE", "ARVALID dropped before ARREADY");
  ar_stable: assert property (@(posedge clk) disable iff (!rst_n) (vif.arvalid && !vif.arready) |=>
                 $stable({vif.arid, vif.araddr, vif.arlen, vif.arsize, vif.arburst, vif.arlock, vif.arcache,
                          vif.arprot, vif.arqos, vif.arregion, vif.aruser}))
    else err("A3.2.1 AR_STABLE", "AR payload changed while ARVALID && !ARREADY");
  ar_vx: assert property (@(posedge clk) disable iff (!rst_n) !$isunknown({vif.arvalid, vif.arready}))
    else err("A3.2.1 AR_HS_X", "ARVALID or ARREADY is X/Z");
  ar_px: assert property (@(posedge clk) disable iff (!rst_n) vif.arvalid |->
                 !$isunknown({vif.arid, vif.araddr, vif.arlen, vif.arsize, vif.arburst, vif.arlock, vif.arcache,
                              vif.arprot, vif.arqos, vif.arregion, vif.aruser}))
    else err("A3.2.1 AR_X", "AR payload has X/Z while ARVALID");
  // R
  r_hold: assert property (@(posedge clk) disable iff (!rst_n) (vif.rvalid && !vif.rready) |=> vif.rvalid)
    else err("A3.2.1 R_VALID_STABLE", "RVALID dropped before RREADY");
  r_stable: assert property (@(posedge clk) disable iff (!rst_n) (vif.rvalid && !vif.rready) |=>
                 $stable({vif.rid, vif.rdata, vif.rresp, vif.rlast, vif.ruser}))
    else err("A3.2.1 R_STABLE", "R payload changed while RVALID && !RREADY");
  r_vx: assert property (@(posedge clk) disable iff (!rst_n) !$isunknown({vif.rvalid, vif.rready}))
    else err("A3.2.1 R_HS_X", "RVALID or RREADY is X/Z");
  r_px: assert property (@(posedge clk) disable iff (!rst_n) vif.rvalid |-> !$isunknown({vif.rid, vif.rresp, vif.rlast, vif.ruser}))
    else err("A3.2.1 R_X", "RID, RRESP, RLAST or RUSER has X/Z while RVALID");

  // ------------------------------------------------- A3.4.1, A4.4, A7.2.4 address channels
  aw_burst: assert property (@(posedge clk) disable iff (!rst_n) vif.awvalid |-> vif.awburst != 2'b11)
    else err("A3.4.1 AW_BURST_RSVD", "AWBURST is the reserved value 0b11");
  aw_wrap_len: assert property (@(posedge clk) disable iff (!rst_n) (vif.awvalid && vif.awburst == 2'b10) |->
                 vif.awlen inside {8'd1, 8'd3, 8'd7, 8'd15})
    else err("A3.4.1 AW_WRAP_LEN", $sformatf("WRAP burst of %0d beats (must be 2, 4, 8 or 16)", vif.awlen + 1));
  aw_wrap_align: assert property (@(posedge clk) disable iff (!rst_n) (vif.awvalid && vif.awburst == 2'b10) |->
                 (vif.awaddr & ((AW'(1) << vif.awsize) - 1)) == 0)
    else err("A3.4.1 AW_WRAP_ALIGN", $sformatf("WRAP start address %0h not aligned to the transfer size", vif.awaddr));
  aw_len16: assert property (@(posedge clk) disable iff (!rst_n) (vif.awvalid && vif.awburst != 2'b01) |-> vif.awlen <= 8'd15)
    else err("A3.4.1 AW_LEN_FIXED_WRAP", $sformatf("FIXED/WRAP burst of %0d beats (at most 16)", vif.awlen + 1));
  aw_size: assert property (@(posedge clk) disable iff (!rst_n) vif.awvalid |-> (1 << vif.awsize) <= SW)
    else err("A3.4.1 AW_SIZE", $sformatf("AWSIZE %0d bytes is wider than the %0d-byte bus", 1 << vif.awsize, SW));
  aw_4k: assert property (@(posedge clk) disable iff (!rst_n) vif.awvalid |->
                 !crosses_4k(vif.awaddr, vif.awlen, vif.awsize, vif.awburst))
    else err("A3.4.1 AW_4K", $sformatf("write burst at %0h crosses a 4 KB boundary", vif.awaddr));
  aw_cache: assert property (@(posedge clk) disable iff (!rst_n) vif.awvalid |-> cache_ok(vif.awcache))
    else err("A4.4 AW_CACHE_RSVD", $sformatf("AWCACHE 0b%04b is reserved", vif.awcache));
  aw_excl: assert property (@(posedge clk) disable iff (!rst_n) (vif.awvalid && vif.awlock) |->
                 excl_ok(vif.awaddr, vif.awlen, vif.awsize))
    else err("A7.2.4 AW_EXCL", "exclusive write: size not a power of two up to 128 bytes, > 16 beats or unaligned");

  ar_burst: assert property (@(posedge clk) disable iff (!rst_n) vif.arvalid |-> vif.arburst != 2'b11)
    else err("A3.4.1 AR_BURST_RSVD", "ARBURST is the reserved value 0b11");
  ar_wrap_len: assert property (@(posedge clk) disable iff (!rst_n) (vif.arvalid && vif.arburst == 2'b10) |->
                 vif.arlen inside {8'd1, 8'd3, 8'd7, 8'd15})
    else err("A3.4.1 AR_WRAP_LEN", $sformatf("WRAP burst of %0d beats (must be 2, 4, 8 or 16)", vif.arlen + 1));
  ar_wrap_align: assert property (@(posedge clk) disable iff (!rst_n) (vif.arvalid && vif.arburst == 2'b10) |->
                 (vif.araddr & ((AW'(1) << vif.arsize) - 1)) == 0)
    else err("A3.4.1 AR_WRAP_ALIGN", $sformatf("WRAP start address %0h not aligned to the transfer size", vif.araddr));
  ar_len16: assert property (@(posedge clk) disable iff (!rst_n) (vif.arvalid && vif.arburst != 2'b01) |-> vif.arlen <= 8'd15)
    else err("A3.4.1 AR_LEN_FIXED_WRAP", $sformatf("FIXED/WRAP burst of %0d beats (at most 16)", vif.arlen + 1));
  ar_size: assert property (@(posedge clk) disable iff (!rst_n) vif.arvalid |-> (1 << vif.arsize) <= SW)
    else err("A3.4.1 AR_SIZE", $sformatf("ARSIZE %0d bytes is wider than the %0d-byte bus", 1 << vif.arsize, SW));
  ar_4k: assert property (@(posedge clk) disable iff (!rst_n) vif.arvalid |->
                 !crosses_4k(vif.araddr, vif.arlen, vif.arsize, vif.arburst))
    else err("A3.4.1 AR_4K", $sformatf("read burst at %0h crosses a 4 KB boundary", vif.araddr));
  ar_cache: assert property (@(posedge clk) disable iff (!rst_n) vif.arvalid |-> cache_ok(vif.arcache))
    else err("A4.4 AR_CACHE_RSVD", $sformatf("ARCACHE 0b%04b is reserved", vif.arcache));
  ar_excl: assert property (@(posedge clk) disable iff (!rst_n) (vif.arvalid && vif.arlock) |->
                 excl_ok(vif.araddr, vif.arlen, vif.arsize))
    else err("A7.2.4 AR_EXCL", "exclusive read: size not a power of two up to 128 bytes, > 16 beats or unaligned");

  // ------------------------------------------------- transaction tracking
  typedef struct {
    logic [IW-1:0] id;
    logic [AW-1:0] addr;
    logic [7:0]    len;
    logic [2:0]    size;
    logic [1:0]    burst;
    logic          lock;
    int unsigned   beats;     // data beats seen so far
    bit            exokay;    // read: an EXOKAY beat seen
    bit            okay;      // read: an OKAY beat seen
  } burst_t;

  typedef struct {
    logic [SW-1:0] strb;
    logic          last;
  } wbeat_t;

  burst_t aw_q[$];      // AW accepted, W burst not finished (W follows AW order)
  wbeat_t w_early[$];   // W beats ahead of their AW
  burst_t wr_done[$];   // AW and all W beats seen, waiting for B
  burst_t rd_q[$];      // AR accepted, R not finished
  bit b_wait, r_wait;   // B / R VALID was HIGH without READY on the previous edge
  int unsigned aw_wait, w_wait, ar_wait, b_wait_n, r_wait_n;

  function automatic int find_id(ref burst_t q[$], input logic [IW-1:0] id);
    foreach (q[i]) if (q[i].id == id) return i;
    return -1;
  endfunction

  // one W beat against the burst at the head of aw_q
  function automatic void w_beat(wbeat_t b);
    burst_t h = aw_q[0];
    logic [SW-1:0] m = lanes(beat_addr(h.addr, h.len, h.size, h.burst, h.beats), h.size);
    bit last_exp = (h.beats == h.len);
    if ((b.strb & ~m) != 0)
      err("A3.4.4 WSTRB_LANES", $sformatf("WSTRB %b outside the lanes %b of beat %0d of the write at %0h",
          b.strb, m, h.beats, h.addr));
    if (b.last !== last_exp)
      err("A3.4.1 WLAST", $sformatf("WLAST=%0b on beat %0d of a %0d-beat write at %0h", b.last, h.beats,
          h.len + 1, h.addr));
    h.beats++;
    if (b.last || last_exp) begin
      void'(aw_q.pop_front());
      wr_done.push_back(h);
    end else
      aw_q[0] = h;
  endfunction

  always @(posedge clk) begin
    if (rst_n !== 1'b1) begin
      aw_q.delete(); w_early.delete(); wr_done.delete(); rd_q.delete();
      b_wait = 0; r_wait = 0;
      aw_wait = 0; w_wait = 0; ar_wait = 0; b_wait_n = 0; r_wait_n = 0;
    end else begin
      // ---- B: checked against the state before this edge's AW/W (A3.3.1)
      if (vif.bvalid === 1'b1) begin
        automatic int i = find_id(wr_done, vif.bid);
        if (!b_wait && i < 0)
          err("A3.3.1 B_BEFORE_WRITE", $sformatf("BVALID with BID %0h before the AW and last W of a write with that ID",
              vif.bid));
        if (!b_wait && i >= 0 && vif.bresp == 2'b01 && !wr_done[i].lock)
          err("A7.2.5 B_EXOKAY", $sformatf("EXOKAY for the non-exclusive write at %0h", wr_done[i].addr));
        if (vif.bready === 1'b1 && i >= 0) wr_done.delete(i);
      end
      b_wait = vif.bvalid === 1'b1 && vif.bready !== 1'b1;

      // ---- R
      if (vif.rvalid === 1'b1) begin
        automatic int i = find_id(rd_q, vif.rid);
        if (i < 0) begin
          if (!r_wait)
            err("A3.3.1 R_BEFORE_AR", $sformatf("RVALID with RID %0h and no outstanding read with that ID", vif.rid));
        end else begin
          automatic burst_t h = rd_q[i];
          automatic logic [SW-1:0] m = lanes(beat_addr(h.addr, h.len, h.size, h.burst, h.beats), h.size);
          if (lanes_x(vif.rdata, m))
            err("A3.4.4 RDATA_X", $sformatf("RDATA has X/Z on the lanes of beat %0d of the read at %0h", h.beats, h.addr));
          if (vif.rready === 1'b1) begin
            automatic bit last_exp = (h.beats == h.len);
            if (vif.rlast !== last_exp)
              err("A3.4.1 RLAST", $sformatf("RLAST=%0b on beat %0d of a %0d-beat read at %0h", vif.rlast, h.beats,
                  h.len + 1, h.addr));
            if (vif.rresp == 2'b01 && !h.lock)
              err("A7.2.5 R_EXOKAY", $sformatf("EXOKAY for the non-exclusive read at %0h", h.addr));
            if (vif.rresp == 2'b01) h.exokay = 1;
            if (vif.rresp == 2'b00) h.okay = 1;
            if (h.lock && h.exokay && h.okay)
              err("A7.2.5 R_EXCL_MIXED", $sformatf("exclusive read at %0h mixes OKAY and EXOKAY beats", h.addr));
            h.beats++;
            if (vif.rlast === 1'b1 || last_exp) rd_q.delete(i);
            else rd_q[i] = h;
          end
        end
      end
      r_wait = vif.rvalid === 1'b1 && vif.rready !== 1'b1;

      // ---- AR
      if (vif.arvalid === 1'b1 && vif.arready === 1'b1) begin
        automatic burst_t t = '{id: vif.arid, addr: vif.araddr, len: vif.arlen, size: vif.arsize, burst: vif.arburst,
                      lock: vif.arlock, beats: 0, exokay: 0, okay: 0};
        rd_q.push_back(t);
      end

      // ---- AW, then any W beats that were waiting for it
      if (vif.awvalid === 1'b1 && vif.awready === 1'b1) begin
        automatic burst_t t = '{id: vif.awid, addr: vif.awaddr, len: vif.awlen, size: vif.awsize, burst: vif.awburst,
                      lock: vif.awlock, beats: 0, exokay: 0, okay: 0};
        aw_q.push_back(t);
        while (aw_q.size() && w_early.size()) w_beat(w_early.pop_front());
      end

      // ---- W
      if (vif.wvalid === 1'b1) begin
        if (lanes_x(vif.wdata, vif.wstrb))
          err("A3.4.4 WDATA_X", "WDATA has X/Z on a strobed lane");
        if (vif.wready === 1'b1) begin
          automatic wbeat_t b = '{strb: vif.wstrb, last: vif.wlast};
          if (aw_q.size()) w_beat(b);
          else w_early.push_back(b);
        end
      end

      // ---- recommendation: bounded wait for READY
      if (MAX_WAIT > 0) begin
        aw_wait  = (vif.awvalid === 1'b1 && vif.awready !== 1'b1) ? aw_wait + 1 : 0;
        w_wait   = (vif.wvalid  === 1'b1 && vif.wready  !== 1'b1) ? w_wait + 1 : 0;
        ar_wait  = (vif.arvalid === 1'b1 && vif.arready !== 1'b1) ? ar_wait + 1 : 0;
        b_wait_n = (vif.bvalid  === 1'b1 && vif.bready  !== 1'b1) ? b_wait_n + 1 : 0;
        r_wait_n = (vif.rvalid  === 1'b1 && vif.rready  !== 1'b1) ? r_wait_n + 1 : 0;
        if (aw_wait  == MAX_WAIT + 1) warn("REC AW_MAX_WAIT", $sformatf("AWREADY low for %0d cycles of AWVALID", MAX_WAIT));
        if (w_wait   == MAX_WAIT + 1) warn("REC W_MAX_WAIT",  $sformatf("WREADY low for %0d cycles of WVALID", MAX_WAIT));
        if (ar_wait  == MAX_WAIT + 1) warn("REC AR_MAX_WAIT", $sformatf("ARREADY low for %0d cycles of ARVALID", MAX_WAIT));
        if (b_wait_n == MAX_WAIT + 1) warn("REC B_MAX_WAIT",  $sformatf("BREADY low for %0d cycles of BVALID", MAX_WAIT));
        if (r_wait_n == MAX_WAIT + 1) warn("REC R_MAX_WAIT",  $sformatf("RREADY low for %0d cycles of RVALID", MAX_WAIT));
      end
    end
  end

  // anything still in flight when the simulation ends
  final begin
    if (aw_q.size() || w_early.size() || wr_done.size() || rd_q.size())
      $display("%s_SVA: at the end %0d writes wait for W, %0d W beats for an AW, %0d writes for B, %0d reads for R",
               NAME, aw_q.size(), w_early.size(), wr_done.size(), rd_q.size());
  end
endmodule
`endif
