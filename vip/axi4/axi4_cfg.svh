// SPDX-License-Identifier: Apache-2.0
// axi4_cfg -- agent configuration
class axi4_err_range extends uvm_object;
  `uvm_object_utils(axi4_err_range)
  axi4_addr_t lo, hi;
  axi4_resp_e resp = AXI4_SLVERR;
  function new(string name = "axi4_err_range");
    super.new(name);
  endfunction
endclass

class axi4_cfg extends uvm_object;
  `uvm_object_utils(axi4_cfg)

  uvm_active_passive_enum is_active = UVM_ACTIVE;
  axi4_role_e   role = AXI4_MASTER;
  int           port = 0;            // tag copied into monitored items
  bit           has_coverage = 1;
  int unsigned  id_width = 8;        // IDs the master may use / slave echoes

  // ---- master knobs (upper bounds of random delays, in clocks) ----
  int unsigned  max_b_delay = 0;     // BREADY low before the response
  int unsigned  max_r_gap   = 0;     // RREADY low between beats
  // bursts in flight at once. 1: finish_item returns when the response is
  // complete (the original behaviour). >1: finish_item returns when the
  // driver has taken the burst; wait for item.done (axi4_api::run does).
  int unsigned  max_outstanding = 1;

  // ---- slave (memory responder) knobs ----
  mem_vip_pkg::mem_model mem;        // storage; created by the agent if null
  int unsigned  max_aready_delay = 0; // AWREADY/ARREADY low cycles
  int unsigned  max_wready_delay = 0; // WREADY low cycles per beat
  int unsigned  max_b_latency    = 0; // cycles from WLAST to BVALID
  int unsigned  max_r_latency    = 0; // cycles from AR to first RVALID
  int unsigned  max_rvalid_gap   = 0; // RVALID low cycles between beats
  axi4_err_range err_ranges[$];       // start address in range -> resp
  // A5/A6: answer bursts with different IDs out of order (same ID stays in
  // order), and interleave read data beats of different IDs
  bit           reorder    = 0;
  bit           interleave = 0;
  // A7.2: exclusive access monitor (one address per ID). 0: the slave does
  // not support exclusive access and answers OKAY (A7.2.3).
  bit           exclusive_support = 1;

  function new(string name = "axi4_cfg");
    super.new(name);
  endfunction

  function void add_err_range(axi4_addr_t lo, axi4_addr_t hi, axi4_resp_e resp = AXI4_SLVERR);
    axi4_err_range r = axi4_err_range::type_id::create("err_range");
    r.lo = lo; r.hi = hi; r.resp = resp;
    err_ranges.push_back(r);
  endfunction

  function axi4_resp_e resp_for(axi4_addr_t addr);
    foreach (err_ranges[i])
      if (addr >= err_ranges[i].lo && addr <= err_ranges[i].hi) return err_ranges[i].resp;
    return AXI4_OKAY;
  endfunction

  // delays are drawn uniformly in [0:max], biased to 0 half of the time
  static function int unsigned rand_delay(int unsigned max);
    if (max == 0 || $urandom_range(1, 0) == 0) return 0;
    return $urandom_range(max, 0);
  endfunction
endclass
