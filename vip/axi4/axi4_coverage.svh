// SPDX-License-Identifier: Apache-2.0
// axi4_coverage -- functional coverage of monitored bursts: direction, burst
// type and length, transfer size up to the bus width, narrow/unaligned and
// partial-strobe transfers, responses, exclusive accesses and their outcome,
// AxCACHE memory types (Table A4-5), AxPROT, QoS and region use.
class axi4_coverage extends uvm_subscriber #(axi4_item);
  `uvm_component_utils(axi4_coverage)

  localparam int MAX_SIZE = $clog2(`AXI4_STRB_W);

  bit          cv_write;
  axi4_burst_e cv_burst;
  int unsigned cv_len;
  bit [2:0]    cv_size;
  axi4_resp_e  cv_resp;
  bit          cv_partial_strb;
  bit          cv_unaligned;
  bit          cv_narrow;
  bit          cv_lock;
  bit [3:0]    cv_cache;
  bit [2:0]    cv_prot;
  bit [3:0]    cv_qos;
  bit [3:0]    cv_region;
  int unsigned n_sampled;

  covergroup cg;
    option.per_instance = 1;
    cp_dir:   coverpoint cv_write { bins rd = {0}; bins wr = {1}; }
    cp_burst: coverpoint cv_burst { bins fixed = {AXI4_FIXED}; bins incr = {AXI4_INCR}; bins wrap = {AXI4_WRAP}; }
    cp_len:   coverpoint cv_len   { bins single = {0}; bins short_b = {[1:3]}; bins mid = {[4:15]};
                                    bins long_b = {[16:255]}; }
    cp_size:  coverpoint cv_size  { bins sz[] = {[0:MAX_SIZE]}; }
    cp_resp:  coverpoint cv_resp  { bins okay = {AXI4_OKAY}; bins exokay = {AXI4_EXOKAY};
                                    bins slverr = {AXI4_SLVERR}; bins decerr = {AXI4_DECERR}; }
    cp_pstrb: coverpoint cv_partial_strb { bins full = {0}; bins partial = {1}; }
    cp_unal:  coverpoint cv_unaligned { bins aligned = {0}; bins unaligned = {1}; }
    cp_narrow: coverpoint cv_narrow { bins full_width = {0}; bins narrow = {1}; }
    cp_lock:  coverpoint cv_lock  { bins normal = {0}; bins exclusive = {1}; }
    cp_cache: coverpoint cv_cache { bins dev_nb = {4'b0000}; bins dev_b = {4'b0001};
                                    bins norm_nc_nb = {4'b0010}; bins norm_nc_b = {4'b0011};
                                    bins wt = {4'b0110, 4'b1010, 4'b1110}; bins wb = {4'b0111, 4'b1011, 4'b1111}; }
    cp_prot:  coverpoint cv_prot  { bins p[] = {[0:7]}; }
    cp_qos:   coverpoint cv_qos   { bins zero = {0}; bins low = {[1:7]}; bins high = {[8:15]}; }
    cp_region: coverpoint cv_region { bins zero = {0}; bins other = {[1:15]}; }
    x_dir_burst: cross cp_dir, cp_burst;
    x_dir_resp:  cross cp_dir, cp_resp;
    x_burst_len: cross cp_burst, cp_len;
    x_lock_resp: cross cp_dir, cp_lock, cp_resp {
      ignore_bins normal_exokay = binsof(cp_lock.normal) && binsof(cp_resp.exokay);
    }
  endgroup

  function new(string name, uvm_component parent);
    super.new(name, parent);
    cg = new();
  endfunction

  function void write(axi4_item t);
    cv_write  = t.write;
    cv_burst  = t.burst;
    cv_len    = t.len;
    cv_size   = t.size;
    cv_resp   = t.worst_resp();
    cv_unaligned = (t.addr & ((axi4_addr_t'(1) << t.size) - 1)) != 0;
    cv_narrow = (1 << t.size) < `AXI4_STRB_W;
    cv_lock   = t.lock;
    cv_cache  = t.cache;
    cv_prot   = t.prot;
    cv_qos    = t.qos;
    cv_region = t.region;
    cv_partial_strb = 0;
    if (t.write) foreach (t.strb[i]) if (t.strb[i] != t.lanes(i)) cv_partial_strb = 1;
    n_sampled++;
    cg.sample();
  endfunction

  function void report_phase(uvm_phase phase);
    `uvm_info("AXI4_COV", $sformatf("%s: %0d bursts sampled, coverage %0.1f%%", get_full_name(),
              n_sampled, cg.get_inst_coverage()), UVM_LOW)
  endfunction
endclass
