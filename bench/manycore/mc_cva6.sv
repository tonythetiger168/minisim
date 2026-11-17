// SPDX-License-Identifier: Apache-2.0
// mc_cva6: OpenHW CVA6 (Ariane) as a gen_manycore.py tile core (--core cva6).
// CVA6 itself is fetched by bench/manycore/fetch_cva6.py; this wrapper builds
// it with that script's configuration and turns its AXI4 master port into the
// tile's held-request port (valid until a one-cycle ready), one 32-bit word at
// a time: a 64-bit beat becomes one access per lane that is in the transfer
// size (reads) or has strobes set (writes), so a narrow load from the
// fetch-and-increment alias is exactly one access. One AXI transaction is in
// flight at a time; reads go before writes.
module mc_cva6 (
  input  logic clk, rst_n,
  output logic req_valid,
  output logic [31:0] req_addr, req_wdata,
  output logic [3:0] req_wstrb,
  input  logic req_ready,
  input  logic [31:0] rdata
);
  localparam config_pkg::cva6_cfg_t Cfg = build_config_pkg::build_config(cva6_config_pkg::cva6_cfg);
  localparam int DW = Cfg.AxiDataWidth;
  localparam int LANES = DW / 32;

  typedef struct packed {
    logic [Cfg.AxiIdWidth-1:0] id; logic [Cfg.AxiAddrWidth-1:0] addr; axi_pkg::len_t len;
    axi_pkg::size_t size; axi_pkg::burst_t burst; logic lock; axi_pkg::cache_t cache;
    axi_pkg::prot_t prot; axi_pkg::qos_t qos; axi_pkg::region_t region; logic [Cfg.AxiUserWidth-1:0] user;
  } ar_t;
  typedef struct packed {
    logic [Cfg.AxiIdWidth-1:0] id; logic [Cfg.AxiAddrWidth-1:0] addr; axi_pkg::len_t len;
    axi_pkg::size_t size; axi_pkg::burst_t burst; logic lock; axi_pkg::cache_t cache;
    axi_pkg::prot_t prot; axi_pkg::qos_t qos; axi_pkg::region_t region; axi_pkg::atop_t atop;
    logic [Cfg.AxiUserWidth-1:0] user;
  } aw_t;
  typedef struct packed {
    logic [DW-1:0] data; logic [DW/8-1:0] strb; logic last; logic [Cfg.AxiUserWidth-1:0] user;
  } w_t;
  typedef struct packed {
    logic [Cfg.AxiIdWidth-1:0] id; axi_pkg::resp_t resp; logic [Cfg.AxiUserWidth-1:0] user;
  } b_t;
  typedef struct packed {
    logic [Cfg.AxiIdWidth-1:0] id; logic [DW-1:0] data; axi_pkg::resp_t resp; logic last;
    logic [Cfg.AxiUserWidth-1:0] user;
  } r_t;
  typedef struct packed {
    aw_t aw; logic aw_valid; w_t w; logic w_valid; logic b_ready; ar_t ar; logic ar_valid; logic r_ready;
  } req_t;
  typedef struct packed {
    logic aw_ready; logic ar_ready; logic w_ready; logic b_valid; b_t b; logic r_valid; r_t r;
  } resp_t;

  req_t  q;
  resp_t p;
  cva6 #(.CVA6Cfg(Cfg), .axi_ar_chan_t(ar_t), .axi_aw_chan_t(aw_t), .axi_w_chan_t(w_t),
         .b_chan_t(b_t), .r_chan_t(r_t), .noc_req_t(req_t), .noc_resp_t(resp_t)) cpu (
    .clk_i(clk), .rst_ni(rst_n), .boot_addr_i('0), .hart_id_i('0), .irq_i('0), .ipi_i(1'b0),
    .time_irq_i(1'b0), .debug_req_i(1'b0), .rvfi_probes_o(), .cvxif_req_o(), .cvxif_resp_i('0),
    .noc_req_o(q), .noc_resp_i(p));

  typedef enum logic [2:0] {IDLE, RD, RBEAT, WDATA, WR, BRESP} st_t;
  st_t st;
  logic [Cfg.AxiIdWidth-1:0] id;
  logic [31:0] a;                 // current beat address
  axi_pkg::len_t left;            // beats after this one
  axi_pkg::size_t size;
  logic [DW-1:0] data;
  logic [DW/8-1:0] strb;
  logic [$clog2(LANES+1)-1:0] lane;

  // lanes a beat touches: by size for reads, by strobes for writes
  function automatic logic lane_used(input int l, input logic rd);
    if (rd) return size >= axi_pkg::size_t'($clog2(DW / 8)) || int'(a[$clog2(DW/8)-1:2]) == l;
    return strb[l*4 +: 4] != 4'd0;
  endfunction

  always_comb begin
    p = '0;
    p.ar_ready = st == IDLE;
    p.aw_ready = st == IDLE && !q.ar_valid;
    p.w_ready = st == WDATA;
    p.r_valid = st == RBEAT;
    p.r.id = id;
    p.r.data = data;
    p.r.last = left == '0;
    p.b_valid = st == BRESP;
    p.b.id = id;
  end

`ifdef MC_TRACE
  always @(posedge clk) begin
    if (q.ar_valid && p.ar_ready) $display("%0t cva6 AR id=%0d a=%h len=%0d size=%0d", $time, q.ar.id, q.ar.addr, q.ar.len, q.ar.size);
    for (int c = 0; c < Cfg.NrCommitPorts; c++)
      if (cpu.commit_ack[c]) $display("%0t cva6 commit pc=%h", $time, cpu.commit_instr_id_commit[c].pc);
    if (cpu.ex_commit.valid) $display("%0t cva6 EXCEPTION cause=%h tval=%h", $time, cpu.ex_commit.cause, cpu.ex_commit.tval);
    if (q.aw_valid && p.aw_ready) $display("%0t cva6 AW id=%0d a=%h len=%0d size=%0d", $time, q.aw.id, q.aw.addr, q.aw.len, q.aw.size);
  end
`endif
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= IDLE;
      req_valid <= 1'b0;
    end else begin
      case (st)
        IDLE:
          if (q.ar_valid) begin
            id <= q.ar.id; a <= q.ar.addr[31:0]; left <= q.ar.len; size <= q.ar.size;
            lane <= '0; st <= RD;
          end else if (q.aw_valid) begin
            id <= q.aw.id; a <= q.aw.addr[31:0]; left <= q.aw.len; size <= q.aw.size;
            st <= WDATA;
          end
        RD:                                     // one word per lane in use, then the R beat
          if (req_valid) begin
            if (req_ready) begin
              req_valid <= 1'b0;
              data[lane*32 +: 32] <= rdata;
              lane <= lane + 1'b1;
            end
          end else if (int'(lane) == LANES) st <= RBEAT;
          else if (lane_used(int'(lane), 1'b1)) begin
            req_valid <= 1'b1;
            req_addr <= {a[31:$clog2(DW/8)], {$clog2(DW/8){1'b0}}} + 32'(lane) * 4;
            req_wstrb <= 4'd0;
          end else lane <= lane + 1'b1;
        RBEAT:
          if (q.r_ready) begin
            if (left == '0) st <= IDLE;
            else begin
              left <= left - 1'b1;
              a <= a + (32'd1 << size);
              lane <= '0;
              st <= RD;
            end
          end
        WDATA:
          if (q.w_valid) begin
            data <= q.w.data; strb <= q.w.strb; lane <= '0; st <= WR;
          end
        WR:
          if (req_valid) begin
            if (req_ready) begin
              req_valid <= 1'b0;
              lane <= lane + 1'b1;
            end
          end else if (int'(lane) == LANES) begin
            if (left == '0) st <= BRESP;
            else begin
              left <= left - 1'b1;
              a <= a + (32'd1 << size);
              st <= WDATA;
            end
          end else if (lane_used(int'(lane), 1'b0)) begin
            req_valid <= 1'b1;
            req_addr <= {a[31:$clog2(DW/8)], {$clog2(DW/8){1'b0}}} + 32'(lane) * 4;
            req_wdata <= data[lane*32 +: 32];
            req_wstrb <= strb[lane*4 +: 4];
          end else lane <= lane + 1'b1;
        BRESP: if (q.b_ready) st <= IDLE;
        default: st <= IDLE;
      endcase
    end
  end
endmodule
