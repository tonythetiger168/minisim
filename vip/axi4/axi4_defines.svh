// SPDX-License-Identifier: Apache-2.0
// axi4_defines -- bus widths of the AXI4 VIP, fixed per compilation.
// Override on the command line, e.g. +define+AXI4_DATA_W=128. The defaults
// keep the original 32-bit address, 32-bit data, 8-bit ID interface.
//   AXI4_ADDR_W  address width            (<= 64)
//   AXI4_DATA_W  data width               (8, 16, 32, ... 1024; IHI0022H A2.3)
//   AXI4_ID_W    ID width (all channels)  (1 .. 32)
//   AXI4_USER_W  width of every xUSER     (1 .. 64)
`ifndef AXI4_DEFINES_SVH
`define AXI4_DEFINES_SVH
`ifndef AXI4_ADDR_W
`define AXI4_ADDR_W 32
`endif
`ifndef AXI4_DATA_W
`define AXI4_DATA_W 32
`endif
`ifndef AXI4_ID_W
`define AXI4_ID_W 8
`endif
`ifndef AXI4_USER_W
`define AXI4_USER_W 8
`endif
`define AXI4_STRB_W (`AXI4_DATA_W / 8)
`endif
