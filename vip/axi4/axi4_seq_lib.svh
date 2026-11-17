// SPDX-License-Identifier: Apache-2.0
// axi4 sequences. Randomization uses $urandom (no constraint solver needed,
// so they run unchanged on simulators with limited randomize() support).

// one burst, fully specified by the caller; response comes back in `item`
class axi4_single_seq extends uvm_sequence #(axi4_item);
  `uvm_object_utils(axi4_single_seq)
  axi4_item item;
  function new(string name = "axi4_single_seq");
    super.new(name);
  endfunction
  task body();
    start_item(item);
    finish_item(item);
    wait (item.done);        // the driver may return early when pipelining
  endtask
endclass

// helpers to build and run bursts from a test or virtual sequence
class axi4_api;
  static function axi4_item make(bit write, axi4_addr_t addr, int unsigned beats = 1,
                                 bit [2:0] size = 2, axi4_burst_e burst = AXI4_INCR,
                                 axi4_id_t id = 0);
    axi4_item t = axi4_item::type_id::create("t");
    t.write = write; t.addr = addr; t.len = beats - 1; t.size = size; t.burst = burst; t.id = id;
    t.cache = 4'b0011;       // Normal Non-cacheable Bufferable (Table A4-5)
    t.alloc();
    for (int unsigned i = 0; i < beats; i++) begin
      for (int w = 0; w < `AXI4_DATA_W; w += 32) t.data[i][w +: 32] = write ? $urandom() : 0;
      t.strb[i] = t.lanes(i);
    end
    return t;
  endfunction

  // runs one burst and returns when its response is complete
  static task run(uvm_sequencer #(axi4_item) sqr, axi4_item t);
    axi4_single_seq s = axi4_single_seq::type_id::create("s");
    s.item = t;
    s.start(sqr);
  endtask

  // 32-bit access at a 4-byte aligned address, on any bus width
  static task write32(uvm_sequencer #(axi4_item) sqr, axi4_addr_t addr, bit [31:0] data,
                      output axi4_resp_e resp, input bit [3:0] strb = 4'hF, input axi4_id_t id = 0);
    axi4_item t = make(1, addr, 1, 2, AXI4_INCR, id);
    int unsigned lane = addr % `AXI4_STRB_W & ~3;
    t.data[0] = '0;
    t.data[0][8*lane +: 32] = data;
    t.strb[0] = '0;
    t.strb[0][lane +: 4] = strb;
    run(sqr, t);
    resp = t.resp[0];
  endtask

  static task read32(uvm_sequencer #(axi4_item) sqr, axi4_addr_t addr, output bit [31:0] data,
                     output axi4_resp_e resp, input axi4_id_t id = 0);
    axi4_item t = make(0, addr, 1, 2, AXI4_INCR, id);
    int unsigned lane = addr % `AXI4_STRB_W & ~3;
    run(sqr, t);
    data = t.data[0][8*lane +: 32];
    resp = t.resp[0];
  endtask

  // A7.2 exclusive pair: an exclusive read, then an exclusive write of the
  // same address, size, length, ID and attributes (random data, returned in
  // t). ok is 1 when the write got EXOKAY.
  static task excl_read(uvm_sequencer #(axi4_item) sqr, axi4_addr_t addr, int unsigned beats,
                        bit [2:0] size, axi4_id_t id, output axi4_item t);
    t = make(0, addr, beats, size, AXI4_INCR, id);
    t.lock = 1;
    run(sqr, t);
  endtask

  static task excl_write(uvm_sequencer #(axi4_item) sqr, axi4_item rd, output bit ok,
                         output axi4_item t);
    t = make(1, rd.addr, rd.beats(), rd.size, rd.burst, rd.id);
    t.lock = 1;
    t.cache = rd.cache; t.prot = rd.prot; t.region = rd.region;
    run(sqr, t);
    ok = t.resp[0] == AXI4_EXOKAY;
  endtask
endclass

// random legal bursts inside [lo, hi] (inclusive byte range). With a driver
// that pipelines (cfg.max_outstanding > 1) several bursts are in flight; a
// burst that overlaps an in-flight burst, one of them a write, waits for it,
// so every read has a single defined result (as a master's hazard logic
// would do; the AXI ordering model gives no order across IDs, A6).
class axi4_random_seq extends uvm_sequence #(axi4_item);
  `uvm_object_utils(axi4_random_seq)

  int unsigned n_items   = 20;
  axi4_addr_t  lo        = 0, hi = 'hFFF;
  int unsigned max_beats = 16;
  bit [2:0]    max_size  = $clog2(`AXI4_STRB_W);
  bit          allow_fixed = 1, allow_wrap = 1, allow_narrow = 1, allow_partial_strb = 1;
  bit          random_attrs = 1;      // random legal AxCACHE/AxPROT/AxQOS/AxREGION/xUSER
  int unsigned excl_pct  = 0;         // % of items that are an exclusive read + write pair
  int unsigned wr_pct    = 50;
  int unsigned id_width  = 4;
  int unsigned max_delay = 2;

  protected axi4_item inflight[$];

  function new(string name = "axi4_random_seq");
    super.new(name);
  endfunction

  protected function void attrs(axi4_item t);
    if (!random_attrs) return;
    t.cache  = AXI4_CACHE_LEGAL[$urandom_range(9, 0)];
    t.prot   = 3'($urandom());
    t.qos    = 4'($urandom());
    t.region = ($urandom_range(3, 0) == 0) ? 4'($urandom()) : 4'h0;
    t.auser  = axi4_user_t'($urandom());
    foreach (t.wuser[i]) t.wuser[i] = axi4_user_t'($urandom());
  endfunction

  // a random legal burst inside [lo, hi] that does not cross 4 KB
  function axi4_item gen();
    axi4_item t;
    int unsigned beats, nb, kind;
    bit [2:0] size;
    axi4_burst_e burst;
    axi4_addr_t a, span;
    for (int tries = 0; tries < 100; tries++) begin
      size = allow_narrow ? 3'($urandom_range(max_size, 0)) : max_size;
      nb = 1 << size;
      kind = $urandom_range(9, 0);
      burst = (allow_wrap && kind == 0) ? AXI4_WRAP : (allow_fixed && kind == 1) ? AXI4_FIXED : AXI4_INCR;
      if (burst == AXI4_WRAP) begin
        int unsigned wl[$] = '{2, 4, 8, 16};
        wl = wl.find(x) with (x <= max_beats);
        if (wl.size() == 0) continue;
        beats = wl[$urandom_range(wl.size() - 1, 0)];
      end else begin
        int unsigned mb = (burst == AXI4_FIXED && max_beats > 16) ? 16 : max_beats;
        beats = (mb > 1 && $urandom_range(3, 0) == 0) ? $urandom_range(mb, 1) :
                $urandom_range(mb < 4 ? mb : 4, 1);
      end
      span = (burst == AXI4_FIXED) ? nb : beats * nb;
      if (hi - lo + 1 < span) continue;
      a = lo + axi4_addr_t'($urandom_range(32'(hi - lo + 1 - span), 0));
      a = a & ~(axi4_addr_t'(nb) - 1);              // aligned start
      if (burst == AXI4_INCR && allow_narrow && $urandom_range(7, 0) == 0)
        a = a + $urandom_range(nb - 1, 0);          // occasional unaligned INCR start
      if (a < lo) continue;
      t = axi4_api::make($urandom_range(99, 0) < wr_pct, a, beats, size, burst,
                         axi4_id_t'($urandom_range((1 << id_width) - 1, 0)));
      if (burst == AXI4_INCR && (t.last_addr() > hi || t.last_addr() >> 12 != a >> 12)) continue;
      if (burst == AXI4_WRAP) begin
        axi4_addr_t wsize = beats * nb;
        axi4_addr_t wbase = a & ~(wsize - 1);
        if (wbase < lo || wbase + wsize - 1 > hi) continue;
      end
      if (t.write && allow_partial_strb)
        foreach (t.strb[i]) if ($urandom_range(3, 0) == 0) t.strb[i] = t.lanes(i) & axi4_strb_t'($urandom());
      attrs(t);
      t.aw_delay = $urandom_range(max_delay, 0);
      t.w_gap    = max_delay;
      t.r_gap    = max_delay;
      if (t.illegal_reason() != "") continue;
      return t;
    end
    `uvm_fatal("AXI4_SEQ", $sformatf("cannot fit a burst in [%0h, %0h]", lo, hi))
    return null;
  endfunction

  // an exclusive read/write pair inside [lo, hi] (A7.2.4 rules)
  function void gen_excl(output axi4_item rd, output axi4_item wr);
    int unsigned bytes, beats, size;
    axi4_addr_t a;
    do begin
      size  = $urandom_range(max_size, 0);
      beats = 1 << $urandom_range(4, 0);
      bytes = beats << size;
    end while (bytes > 128 || bytes > hi - lo + 1);
    a = lo + axi4_addr_t'($urandom_range(32'(hi - lo + 1 - bytes), 0));
    a = (a + bytes - 1) & ~(axi4_addr_t'(bytes) - 1);
    if (a + bytes - 1 > hi) a -= bytes;
    rd = axi4_api::make(0, a, beats, size, AXI4_INCR, axi4_id_t'($urandom_range((1 << id_width) - 1, 0)));
    rd.lock = 1;
    attrs(rd);
    rd.cache = 4'b0010;                               // A7.2.4: reaches the monitor
    wr = axi4_api::make(1, a, beats, size, AXI4_INCR, rd.id);
    wr.lock = 1;
    wr.cache = rd.cache; wr.prot = rd.prot; wr.region = rd.region; wr.qos = rd.qos; wr.auser = rd.auser;
  endfunction

  protected function bit conflicts(axi4_item t);
    foreach (inflight[i])
      if (!inflight[i].done && (t.write || inflight[i].write) && t.overlaps(inflight[i])) return 1;
    return 0;
  endfunction

  protected task issue(axi4_item t);
    inflight = inflight.find(x) with (!x.done);
    while (conflicts(t)) begin
      #1;
      inflight = inflight.find(x) with (!x.done);
    end
    start_item(t);
    finish_item(t);
    inflight.push_back(t);
  endtask

  task body();
    repeat (n_items) begin
      if (excl_pct && $urandom_range(99, 0) < excl_pct) begin
        axi4_item rd, wr;
        gen_excl(rd, wr);
        issue(rd);
        wait (rd.done);                               // A7.2.2: write after the read completes
        issue(wr);
      end else
        issue(gen());
    end
    foreach (inflight[i]) wait (inflight[i].done);
  endtask
endclass
