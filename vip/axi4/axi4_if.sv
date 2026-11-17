// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// axi4_if -- AXI4 interface with the full AXI4 signal set of IHI0022H
// chapter A2 and A8: AxQOS, AxREGION and the five xUSER signals included.
// ----------------------------------------------------------------------------
// Widths come from axi4_defines.svh (+define+AXI4_DATA_W=... etc.). A DUT
// port with a narrower ID or USER uses the low bits and the testbench ties
// the rest to 0; a DUT without QOS/REGION/USER leaves them at their A9.3
// defaults (0). Clocking blocks:
//   mst_cb  manager (master) side     slv_cb  subordinate (slave) side
//   mon_cb  passive sampling
// ============================================================================
`ifndef AXI4_IF_SV
`define AXI4_IF_SV
`include "axi4_defines.svh"
interface axi4_if (input logic aclk, input logic aresetn);
  localparam int AW = `AXI4_ADDR_W;
  localparam int DW = `AXI4_DATA_W;
  localparam int IW = `AXI4_ID_W;
  localparam int UW = `AXI4_USER_W;

  // write address (A2.2)
  logic [IW-1:0]   awid;
  logic [AW-1:0]   awaddr;
  logic [7:0]      awlen;
  logic [2:0]      awsize;
  logic [1:0]      awburst;
  logic            awlock;
  logic [3:0]      awcache;
  logic [2:0]      awprot;
  logic [3:0]      awqos;
  logic [3:0]      awregion;
  logic [UW-1:0]   awuser;
  logic            awvalid;
  logic            awready;
  // write data (A2.3)
  logic [DW-1:0]   wdata;
  logic [DW/8-1:0] wstrb;
  logic            wlast;
  logic [UW-1:0]   wuser;
  logic            wvalid;
  logic            wready;
  // write response (A2.4)
  logic [IW-1:0]   bid;
  logic [1:0]      bresp;
  logic [UW-1:0]   buser;
  logic            bvalid;
  logic            bready;
  // read address (A2.5)
  logic [IW-1:0]   arid;
  logic [AW-1:0]   araddr;
  logic [7:0]      arlen;
  logic [2:0]      arsize;
  logic [1:0]      arburst;
  logic            arlock;
  logic [3:0]      arcache;
  logic [2:0]      arprot;
  logic [3:0]      arqos;
  logic [3:0]      arregion;
  logic [UW-1:0]   aruser;
  logic            arvalid;
  logic            arready;
  // read data (A2.6)
  logic [IW-1:0]   rid;
  logic [DW-1:0]   rdata;
  logic [1:0]      rresp;
  logic            rlast;
  logic [UW-1:0]   ruser;
  logic            rvalid;
  logic            rready;

  clocking mst_cb @(posedge aclk);
    default input #1step output #1;
    output awid, awaddr, awlen, awsize, awburst, awlock, awcache, awprot, awqos, awregion, awuser, awvalid;
    input  awready;
    output wdata, wstrb, wlast, wuser, wvalid;
    input  wready;
    input  bid, bresp, buser, bvalid;
    output bready;
    output arid, araddr, arlen, arsize, arburst, arlock, arcache, arprot, arqos, arregion, aruser, arvalid;
    input  arready;
    input  rid, rdata, rresp, rlast, ruser, rvalid;
    output rready;
  endclocking

  clocking slv_cb @(posedge aclk);
    default input #1step output #1;
    input  awid, awaddr, awlen, awsize, awburst, awlock, awcache, awprot, awqos, awregion, awuser, awvalid;
    output awready;
    input  wdata, wstrb, wlast, wuser, wvalid;
    output wready;
    output bid, bresp, buser, bvalid;
    input  bready;
    input  arid, araddr, arlen, arsize, arburst, arlock, arcache, arprot, arqos, arregion, aruser, arvalid;
    output arready;
    output rid, rdata, rresp, rlast, ruser, rvalid;
    input  rready;
  endclocking

  clocking mon_cb @(posedge aclk);
    default input #1step;
    input awid, awaddr, awlen, awsize, awburst, awlock, awcache, awprot, awqos, awregion, awuser, awvalid, awready;
    input wdata, wstrb, wlast, wuser, wvalid, wready;
    input bid, bresp, buser, bvalid, bready;
    input arid, araddr, arlen, arsize, arburst, arlock, arcache, arprot, arqos, arregion, aruser, arvalid, arready;
    input rid, rdata, rresp, rlast, ruser, rvalid, rready;
  endclocking
endinterface
`endif
