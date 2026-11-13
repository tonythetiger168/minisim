// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// axi4_sva_tb -- negative test of the AXI4 protocol checker (axi4_sva).
// The module drives the AXI4 wires directly, one scenario per rule: each
// scenario breaks exactly one rule of IHI0022H.c, between resets that clear
// the checker's transaction state. The UVM test (axi4_sva_neg_test) catches
// the checker's reports and passes when every expected rule fired and no
// other one did. The last scenarios are legal traffic and must stay silent.
// ============================================================================
`timescale 1ns/1ps
`include "axi4_defines.svh"
package axi4_sva_test_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"

  // rules each scenario must trigger, in scenario order
  const string EXPECTED[$] = '{
    "A3.1.2 VALID_IN_RESET", "A3.1.2 VALID_AT_RESET_EXIT",
    "A3.2.1 AW_VALID_STABLE", "A3.2.1 AW_STABLE", "A3.2.1 AW_HS_X", "A3.2.1 AW_X",
    "A3.4.1 AW_BURST_RSVD", "A3.4.1 AW_WRAP_LEN", "A3.4.1 AW_WRAP_ALIGN", "A3.4.1 AW_LEN_FIXED_WRAP",
    "A3.4.1 AW_SIZE", "A3.4.1 AW_4K", "A4.4 AW_CACHE_RSVD", "A7.2.4 AW_EXCL",
    "A3.2.1 W_VALID_STABLE", "A3.2.1 W_STABLE", "A3.2.1 W_X", "A3.4.4 WDATA_X",
    "A3.4.4 WSTRB_LANES", "A3.4.1 WLAST",
    "A3.3.1 B_BEFORE_WRITE", "A3.2.1 B_STABLE", "A3.2.1 B_VALID_STABLE", "A3.2.1 B_X", "A7.2.5 B_EXOKAY",
    "A3.2.1 AR_VALID_STABLE", "A3.2.1 AR_STABLE", "A3.2.1 AR_X",
    "A3.4.1 AR_BURST_RSVD", "A3.4.1 AR_WRAP_LEN", "A3.4.1 AR_WRAP_ALIGN", "A3.4.1 AR_LEN_FIXED_WRAP",
    "A3.4.1 AR_SIZE", "A3.4.1 AR_4K", "A4.4 AR_CACHE_RSVD", "A7.2.4 AR_EXCL",
    "A3.3.1 R_BEFORE_AR", "A3.2.1 R_VALID_STABLE", "A3.2.1 R_STABLE", "A3.2.1 R_X",
    "A3.4.4 RDATA_X", "A3.4.1 RLAST", "A7.2.5 R_EXOKAY", "A7.2.5 R_EXCL_MIXED",
    "A3.2.1 W_HS_X", "A3.2.1 B_HS_X", "A3.2.1 AR_HS_X", "A3.2.1 R_HS_X",
    "REC AR_MAX_WAIT"
  };

  class sva_catcher extends uvm_report_catcher;
    int unsigned seen[string];
    string order[$];
    function new(string name = "sva_catcher");
      super.new(name);
    endfunction
    function action_e catch();
      string m = get_message();
      if (get_id() == "axi_SVA" && m.len() > 2 && m[0] == "[") begin
        int e = 1;
        while (e < m.len() && m[e] != "]") e++;
        begin
          string rule = m.substr(1, e - 1);
          if (!seen.exists(rule)) order.push_back(rule);
          seen[rule]++;
        end
        return CAUGHT;
      end
      return THROW;
    endfunction
  endclass

  class axi4_sva_neg_test extends uvm_test;
    `uvm_component_utils(axi4_sva_neg_test)
    sva_catcher c;
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    function void build_phase(uvm_phase phase);
      c = new("c");
      uvm_report_cb::add(null, c);
    endfunction
    task run_phase(uvm_phase phase);
      uvm_event done = uvm_event_pool::get_global("axi4_sva_done");
      phase.raise_objection(this);
      done.wait_trigger();
      phase.drop_objection(this);
    endtask
    function void check_phase(uvm_phase phase);
      int unsigned checks = 0, bad = 0;
      foreach (EXPECTED[i]) begin
        checks++;
        if (!c.seen.exists(EXPECTED[i])) begin
          bad++;
          `uvm_error("NEG", {"rule did not fire: ", EXPECTED[i]})
        end
      end
      foreach (c.order[i]) begin
        bit known = 0;
        checks++;
        foreach (EXPECTED[j]) if (EXPECTED[j] == c.order[i]) known = 1;
        if (!known) begin
          bad++;
          `uvm_error("NEG", $sformatf("unexpected rule fired %0d times: %s", c.seen[c.order[i]], c.order[i]))
        end
      end
      `uvm_info("NEG", $sformatf("%0d rules expected, %0d fired", EXPECTED.size(), c.seen.num()), UVM_NONE)
      if (bad == 0) $display("SB_PASS checks=%0d", checks);
    endfunction
  endclass
endpackage

module axi4_sva_tb;
  import uvm_pkg::*;
  import axi4_sva_test_pkg::*;
  `include "uvm_macros.svh"

  logic clk = 1'b0, rst_n = 1'b0;
  always #5 clk = ~clk;

  axi4_if bus (.aclk(clk), .aresetn(rst_n));
  axi4_sva #(.NAME("axi"), .MAX_WAIT(4)) u_sva (bus);

  localparam int SW = `AXI4_DATA_W / 8;

  // everything idle and known
  task automatic idle();
    bus.awvalid = 0; bus.wvalid = 0; bus.bvalid = 0; bus.arvalid = 0; bus.rvalid = 0;
    bus.awready = 0; bus.wready = 0; bus.bready = 0; bus.arready = 0; bus.rready = 0;
    bus.awid = 0; bus.awaddr = 0; bus.awlen = 0; bus.awsize = 2; bus.awburst = 1; bus.awlock = 0;
    bus.awcache = 0; bus.awprot = 0; bus.awqos = 0; bus.awregion = 0; bus.awuser = 0;
    bus.wdata = 0; bus.wstrb = '1; bus.wlast = 0; bus.wuser = 0;
    bus.bid = 0; bus.bresp = 0; bus.buser = 0;
    bus.arid = 0; bus.araddr = 0; bus.arlen = 0; bus.arsize = 2; bus.arburst = 1; bus.arlock = 0;
    bus.arcache = 0; bus.arprot = 0; bus.arqos = 0; bus.arregion = 0; bus.aruser = 0;
    bus.rid = 0; bus.rdata = 0; bus.rresp = 0; bus.rlast = 0; bus.ruser = 0;
  endtask

  // signals change on the falling edge, the checker samples on the rising one
  task automatic cyc(int n = 1);
    repeat (n) @(negedge clk);
  endtask

  // a reset between scenarios clears the checker's state
  task automatic reset();
    idle();
    rst_n = 0;
    cyc(2);
    rst_n = 1;
    cyc(2);
  endtask

  // one-cycle AW handshake with the current AW payload
  task automatic aw_hs();
    bus.awvalid = 1; bus.awready = 1; cyc(); bus.awvalid = 0; bus.awready = 0;
  endtask
  task automatic ar_hs();
    bus.arvalid = 1; bus.arready = 1; cyc(); bus.arvalid = 0; bus.arready = 0;
  endtask
  task automatic w_hs(logic last);
    bus.wvalid = 1; bus.wready = 1; bus.wlast = last; cyc(); bus.wvalid = 0; bus.wready = 0; bus.wlast = 0;
  endtask
  task automatic b_hs(logic [1:0] resp = 0);
    bus.bvalid = 1; bus.bready = 1; bus.bresp = resp; cyc(); bus.bvalid = 0; bus.bready = 0; bus.bresp = 0;
  endtask
  task automatic r_hs(logic last, logic [1:0] resp = 0);
    bus.rvalid = 1; bus.rready = 1; bus.rlast = last; bus.rresp = resp; cyc();
    bus.rvalid = 0; bus.rready = 0; bus.rlast = 0; bus.rresp = 0;
  endtask

  initial run_test();

  initial begin
    uvm_event done = uvm_event_pool::get_global("axi4_sva_done");
    idle();
    cyc(3);
    // ---- A3.1.2 reset
    bus.awvalid = 1; cyc(); bus.awvalid = 0;                     // VALID_IN_RESET
    cyc();
    rst_n = 1; bus.arvalid = 1; bus.arready = 1; cyc();          // VALID_AT_RESET_EXIT
    bus.arvalid = 0; bus.arready = 0; bus.rvalid = 1; bus.rready = 1; bus.rlast = 1; cyc();
    reset();
    // ---- AW handshake rules
    bus.awvalid = 1; cyc(); bus.awvalid = 0; cyc(); reset();            // AW_VALID_STABLE
    bus.awvalid = 1; cyc(); bus.awaddr = 'h40; cyc(); aw_hs(); reset();   // AW_STABLE
    bus.awready = 'x; cyc(); reset();                            // AW_HS_X
    bus.awid = 'x; aw_hs(); reset();                             // AW_X
    // ---- AW burst rules
    bus.awburst = 3; aw_hs(); reset();                           // AW_BURST_RSVD
    bus.awburst = 2; bus.awlen = 2; aw_hs(); reset();            // AW_WRAP_LEN
    bus.awburst = 2; bus.awlen = 3; bus.awaddr = 2; aw_hs(); reset();     // AW_WRAP_ALIGN
    bus.awburst = 0; bus.awlen = 16; aw_hs(); reset();           // AW_LEN_FIXED_WRAP
    bus.awsize = $clog2(SW) + 1; bus.awaddr = 0; aw_hs(); reset();        // AW_SIZE
    bus.awaddr = 'hFFC; bus.awlen = 1; aw_hs(); reset();         // AW_4K
    bus.awcache = 4'b0100; aw_hs(); reset();                     // AW_CACHE_RSVD
    bus.awlock = 1; bus.awlen = 2; aw_hs(); reset();             // AW_EXCL (12 bytes)
    // ---- W rules
    bus.wvalid = 1; cyc(); bus.wvalid = 0; cyc(); reset();              // W_VALID_STABLE
    bus.wvalid = 1; cyc(); bus.wdata = 1; cyc(); w_hs(1); reset();        // W_STABLE
    bus.wstrb = 'x; w_hs(1); reset();                            // W_X
    bus.wdata = 'x; bus.wstrb = 1; w_hs(1); reset();             // WDATA_X
    bus.awsize = 0; aw_hs(); bus.wstrb = 'b11; w_hs(1); reset(); // WSTRB_LANES (1-byte beat, 2 strobes)
    bus.awlen = 1; aw_hs(); w_hs(1); reset();                    // WLAST on beat 0 of 2
    // ---- B rules
    bus.bid = 5; b_hs(); reset();                                // B_BEFORE_WRITE
    aw_hs(); w_hs(1); bus.bvalid = 1; cyc(); bus.bresp = 2; cyc(); b_hs(2); reset();  // B_STABLE
    aw_hs(); w_hs(1); bus.bvalid = 1; cyc(); bus.bvalid = 0; cyc(); reset();     // B_VALID_STABLE
    aw_hs(); w_hs(1); bus.buser = 'x; b_hs(); reset();           // B_X
    aw_hs(); w_hs(1); b_hs(1); reset();                          // B_EXOKAY
    // ---- AR rules
    bus.arvalid = 1; cyc(); bus.arvalid = 0; cyc(); reset();            // AR_VALID_STABLE
    bus.arvalid = 1; cyc(); bus.arlen = 3; cyc(); ar_hs(); reset();       // AR_STABLE
    bus.arqos = 'x; ar_hs(); reset();                            // AR_X
    bus.arburst = 3; ar_hs(); reset();                           // AR_BURST_RSVD
    bus.arburst = 2; bus.arlen = 4; ar_hs(); reset();            // AR_WRAP_LEN
    bus.arburst = 2; bus.arlen = 1; bus.araddr = 1; ar_hs(); reset();     // AR_WRAP_ALIGN
    bus.arburst = 0; bus.arlen = 20; ar_hs(); reset();           // AR_LEN_FIXED_WRAP
    bus.arsize = $clog2(SW) + 1; ar_hs(); reset();               // AR_SIZE
    bus.araddr = 'h1FF8; bus.arlen = 3; ar_hs(); reset();        // AR_4K
    bus.arcache = 4'b1001; ar_hs(); reset();                     // AR_CACHE_RSVD
    bus.arlock = 1; bus.araddr = 4; bus.arlen = 1; ar_hs(); reset();      // AR_EXCL (8 bytes at 4)
    // ---- R rules
    bus.rid = 3; r_hs(1); reset();                               // R_BEFORE_AR
    ar_hs(); bus.rvalid = 1; bus.rlast = 1; cyc(); bus.rvalid = 0; cyc(); reset();   // R_VALID_STABLE
    ar_hs(); bus.rvalid = 1; bus.rlast = 1; cyc(); bus.rresp = 2; cyc(); r_hs(1, 2); reset();  // R_STABLE
    ar_hs(); bus.ruser = 'x; r_hs(1); reset();                   // R_X
    ar_hs(); bus.rdata = 'x; r_hs(1); reset();                   // RDATA_X
    bus.arlen = 1; ar_hs(); r_hs(1); reset();                    // RLAST on beat 0 of 2
    ar_hs(); r_hs(1, 1); reset();                                // R_EXOKAY
    bus.arlock = 1; bus.arlen = 1; ar_hs(); r_hs(0, 1); r_hs(1, 0); reset();  // R_EXCL_MIXED
    // ---- READY X on the other channels
    bus.wready = 'x; cyc(); reset();                             // W_HS_X
    bus.bready = 'x; cyc(); reset();                             // B_HS_X
    bus.arready = 'x; cyc(); reset();                            // AR_HS_X
    bus.rready = 'x; cyc(); reset();                             // R_HS_X
    // ---- recommendation: ARREADY low for more than MAX_WAIT cycles
    bus.arvalid = 1; cyc(6); ar_hs(); bus.rvalid = 1; bus.rready = 1; bus.rlast = 1; cyc(); reset();
    // ---- legal traffic: must not fire anything
    // W before AW, narrow unaligned INCR, partial strobes, two IDs out of order
    bus.awid = 1; bus.awaddr = 'h101; bus.awlen = 1; bus.awsize = 1;
    bus.wstrb = 1 << ('h101 % SW); w_hs(0);
    bus.wstrb = 3 << ('h102 % SW); w_hs(1);
    aw_hs();
    bus.awid = 2; bus.awaddr = 'h200; bus.awlen = 0; bus.awsize = 2; bus.wstrb = 'hF; aw_hs(); w_hs(1);
    bus.bid = 2; b_hs(); bus.bid = 1; b_hs();
    // exclusive pair answered EXOKAY, interleaved with a normal read of another ID
    bus.arid = 4; bus.araddr = 'h300; bus.arlen = 1; bus.arsize = 2; bus.arlock = 1; ar_hs();
    bus.arid = 5; bus.araddr = 'h400; bus.arlen = 1; bus.arlock = 0; bus.arburst = 2; ar_hs();
    bus.rid = 5; r_hs(0); bus.rid = 4; r_hs(0, 1); bus.rid = 5; r_hs(1); bus.rid = 4; r_hs(1, 1);
    bus.awid = 4; bus.awaddr = 'h300; bus.awlen = 1; bus.awlock = 1; aw_hs(); w_hs(0); w_hs(1);
    bus.bid = 4; b_hs(1);
    cyc(3);
    done.trigger();
  end

  initial begin
    #1ms;
    `uvm_fatal("TB", "watchdog timeout")
  end
endmodule
