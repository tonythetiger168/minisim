// SPDX-License-Identifier: Apache-2.0
// axi4_item -- one AXI4 burst (request + response), used by drivers, monitors
// and sequences. Per-beat arrays have len+1 entries; for a write, resp[0] is
// BRESP. Section numbers refer to IHI0022H.c.
class axi4_item extends uvm_sequence_item;
  `uvm_object_utils(axi4_item)

  rand bit          write;
  rand axi4_id_t    id;
  rand axi4_addr_t  addr;
  rand bit [7:0]    len;        // AxLEN: beats - 1
  rand bit [2:0]    size;       // AxSIZE: bytes per beat = 1 << size
  rand axi4_burst_e burst;
  bit               lock;       // AxLOCK: exclusive access (A7.2)
  bit [3:0]         cache;      // AxCACHE (A4.4)
  bit [2:0]         prot;       // AxPROT (A4.7)
  bit [3:0]         qos;        // AxQOS (A8.1)
  bit [3:0]         region;     // AxREGION (A8.2)
  axi4_user_t       auser;      // AxUSER (A8.3)
  axi4_data_t       data[];     // write data or read data, one per beat
  axi4_strb_t       strb[];     // write strobes (reads: lanes the beat covers)
  axi4_user_t       wuser[];    // WUSER per beat
  axi4_resp_e       resp[];     // write: [0] = BRESP; read: RRESP per beat
  axi4_user_t       buser;      // BUSER
  axi4_user_t       ruser[];    // RUSER per beat

  // filled in by monitors
  int               port = -1;  // agent tag (e.g. interconnect master index)
  time              t_start, t_end;

  // knobs read by the master driver
  int unsigned      aw_delay, w_gap, b_delay, r_gap;

  // set by the master driver when the response is complete
  bit               done;
  int unsigned      n_rbeats;   // read beats received so far (driver use)

  function new(string name = "axi4_item");
    super.new(name);
  endfunction

  function int unsigned beats();
    return int'(len) + 1;
  endfunction

  // allocate the per-beat arrays for the current len
  function void alloc();
    data  = new[beats()];
    strb  = new[beats()];
    wuser = new[beats()];
    resp  = new[write ? 1 : beats()];
    ruser = new[write ? 0 : beats()];
  endfunction

  // A3.4.1 / A3.4.2 address of beat i
  static function axi4_addr_t beat_addr_of(axi4_addr_t addr, bit [7:0] len, bit [2:0] size,
                                           axi4_burst_e burst, int unsigned i);
    axi4_addr_t nb = axi4_addr_t'(1) << size;
    axi4_addr_t aligned = addr & ~(nb - 1);
    axi4_addr_t wsize, wbase;
    case (burst)
      AXI4_FIXED: return addr;
      AXI4_WRAP: begin
        wsize = nb * (axi4_addr_t'(len) + 1);
        wbase = addr & ~(wsize - 1);
        return wbase + ((aligned - wbase + axi4_addr_t'(i) * nb) % wsize);
      end
      default: return (i == 0) ? addr : aligned + axi4_addr_t'(i) * nb;
    endcase
  endfunction

  function axi4_addr_t beat_addr(int unsigned i);
    return beat_addr_of(addr, len, size, burst, i);
  endfunction

  // A3.4.4 byte lanes of the bus that a beat at address a with this size uses
  static function axi4_strb_t lanes_of(axi4_addr_t a, bit [2:0] size);
    int unsigned nb = 1 << size;
    int unsigned lo = a % `AXI4_STRB_W;
    int unsigned hi = (a & ~(axi4_addr_t'(nb) - 1)) % `AXI4_STRB_W + nb - 1;
    axi4_strb_t m = '0;
    for (int unsigned b = lo; b <= hi && b < `AXI4_STRB_W; b++) m[b] = 1'b1;
    return m;
  endfunction

  function axi4_strb_t lanes(int unsigned i);
    return lanes_of(beat_addr(i), size);
  endfunction

  // last byte address of an INCR burst (4 KB crossing checks)
  function axi4_addr_t last_addr();
    return beat_addr(len) | ((axi4_addr_t'(1) << size) - 1);
  endfunction

  // byte range the burst touches, [lo, hi] (hazard checks)
  function void span(output axi4_addr_t lo, output axi4_addr_t hi);
    axi4_addr_t nb = axi4_addr_t'(1) << size;
    case (burst)
      AXI4_FIXED: begin lo = addr & ~(nb - 1); hi = lo + nb - 1; end
      AXI4_WRAP: begin
        axi4_addr_t ws = nb * (axi4_addr_t'(len) + 1);
        lo = addr & ~(ws - 1); hi = lo + ws - 1;
      end
      default: begin lo = addr & ~(nb - 1); hi = last_addr(); end
    endcase
  endfunction

  function bit overlaps(axi4_item o);
    axi4_addr_t a0, a1, b0, b1;
    span(a0, a1);
    o.span(b0, b1);
    return !(a1 < b0 || b1 < a0);
  endfunction

  // "" if the request obeys the AXI4 rules axi4_sva checks on the address
  // channel, else the first rule it breaks
  function string illegal_reason();
    int unsigned nbytes = beats() << size;
    if ((1 << size) > `AXI4_STRB_W) return "A3.4.1 size wider than the bus";
    if (burst == AXI4_WRAP && !(len inside {1, 3, 7, 15})) return "A3.4.1 WRAP length";
    if (burst == AXI4_WRAP && (addr & ((axi4_addr_t'(1) << size) - 1)) != 0) return "A3.4.1 WRAP alignment";
    if (burst != AXI4_INCR && len > 15) return "A3.4.1 FIXED/WRAP longer than 16";
    if (burst == AXI4_INCR && last_addr() >> 12 != addr >> 12) return "A3.4.1 4 KB crossing";
    if (!cache[1] && cache[3:2] != 0) return "A4.4 reserved AxCACHE";
    if (lock && (len > 15 || nbytes > 128 || (nbytes & (nbytes - 1)) != 0 ||
                 (addr & axi4_addr_t'(nbytes - 1)) != 0))
      return "A7.2.4 exclusive size, length or alignment";
    return "";
  endfunction

  function axi4_resp_e worst_resp();
    axi4_resp_e w = AXI4_OKAY;
    foreach (resp[i]) if (resp[i] > w) w = resp[i];
    return w;
  endfunction

  // A7.2.5: the access was exclusive and every beat answered EXOKAY
  function bit excl_ok();
    if (!lock || resp.size() == 0) return 0;
    foreach (resp[i]) if (resp[i] != AXI4_EXOKAY) return 0;
    return 1;
  endfunction

  virtual function void do_copy(uvm_object rhs);
    axi4_item r;
    super.do_copy(rhs);
    if (!$cast(r, rhs)) `uvm_fatal("AXI4_ITEM", "do_copy: type mismatch")
    write = r.write; id = r.id; addr = r.addr; len = r.len; size = r.size; burst = r.burst;
    lock = r.lock; cache = r.cache; prot = r.prot; qos = r.qos; region = r.region; auser = r.auser;
    data = r.data; strb = r.strb; wuser = r.wuser; resp = r.resp; buser = r.buser; ruser = r.ruser;
    port = r.port; t_start = r.t_start; t_end = r.t_end;
    aw_delay = r.aw_delay; w_gap = r.w_gap; b_delay = r.b_delay; r_gap = r.r_gap;
  endfunction

  virtual function string convert2string();
    string s;
    s = $sformatf("%s id=%0h addr=%0h len=%0d size=%0d %s", write ? "WR" : "RD", id, addr,
                  len, size, burst.name());
    if (lock)   s = {s, " EXCL"};
    if (cache)  s = {s, $sformatf(" cache=%04b", cache)};
    if (prot)   s = {s, $sformatf(" prot=%03b", prot)};
    if (qos)    s = {s, $sformatf(" qos=%0d", qos)};
    if (region) s = {s, $sformatf(" region=%0d", region)};
    if (resp.size()) s = {s, " resp=", worst_resp().name()};
    if (data.size()) s = {s, $sformatf(" data[0]=%0h", data[0])};
    if (port >= 0)   s = {s, $sformatf(" port=%0d", port)};
    return s;
  endfunction
endclass
