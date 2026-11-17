// SPDX-License-Identifier: Apache-2.0
// ============================================================================
// axi4_pkg -- AXI4 UVM VIP: item, config, master driver (several outstanding
// bursts), slave memory responder (exclusive monitor, out-of-order and
// interleaved responses), monitor, coverage, agent, sequences.
// Interface: axi4_if.sv; protocol checker: axi4_sva.sv; widths:
// axi4_defines.svh. Section numbers refer to IHI0022H.c (spec/README.md).
// ============================================================================
`ifndef AXI4_PKG_SV
`define AXI4_PKG_SV
`include "axi4_defines.svh"
package axi4_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"

  // the widths of axi4_defines.svh, for code outside this compilation unit
  localparam int AXI4_ADDR_WIDTH = `AXI4_ADDR_W;
  localparam int AXI4_DATA_WIDTH = `AXI4_DATA_W;
  localparam int AXI4_STRB_WIDTH = `AXI4_STRB_W;
  localparam int AXI4_ID_WIDTH   = `AXI4_ID_W;
  localparam int AXI4_USER_WIDTH = `AXI4_USER_W;

  typedef bit [`AXI4_ADDR_W-1:0] axi4_addr_t;
  typedef bit [`AXI4_DATA_W-1:0] axi4_data_t;
  typedef bit [`AXI4_STRB_W-1:0] axi4_strb_t;
  typedef bit [`AXI4_ID_W-1:0]   axi4_id_t;
  typedef bit [`AXI4_USER_W-1:0] axi4_user_t;

  typedef enum bit [1:0] { AXI4_FIXED = 2'b00, AXI4_INCR = 2'b01, AXI4_WRAP = 2'b10 } axi4_burst_e;
  typedef enum bit [1:0] { AXI4_OKAY = 2'b00, AXI4_EXOKAY = 2'b01,
                           AXI4_SLVERR = 2'b10, AXI4_DECERR = 2'b11 } axi4_resp_e;
  typedef enum int { AXI4_MASTER = 0, AXI4_SLAVE = 1 } axi4_role_e;

  // the AxCACHE values of Table A4-5 (A4.4); the others are reserved
  const bit [3:0] AXI4_CACHE_LEGAL[10] = '{4'b0000, 4'b0001, 4'b0010, 4'b0011, 4'b0110,
                                           4'b0111, 4'b1010, 4'b1011, 4'b1110, 4'b1111};

  `include "axi4_item.svh"
  `include "axi4_cfg.svh"
  `include "axi4_master_driver.svh"
  `include "axi4_slave_driver.svh"
  `include "axi4_monitor.svh"
  `include "axi4_coverage.svh"
  `include "axi4_agent.svh"
  `include "axi4_seq_lib.svh"
endpackage
`endif
