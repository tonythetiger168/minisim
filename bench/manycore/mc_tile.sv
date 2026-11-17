// SPDX-License-Identifier: Apache-2.0
// mc_tile: one tile of the bench/gen_manycore.py many-core: a RISC-V core,
// 4 KB of private RAM holding the program (the same image in every tile),
// a few memory-mapped registers, the private L1 data cache, this tile's slice
// of the shared L2 with its directory, and one router per network (NoC1
// requests, NoC2 responses and invalidations, NoC3 invalidation acks).
// Tile coordinates come in on ports, as chip straps would, so every tile of a
// core type is the same module with the same parameters.
//
// Core address map:
//   0x0000_0000-0x0000_0FFF  private RAM (code and per-core constants)
//   0x8000_0000-0x8FFF_FFFF  shared memory, cached in L1, homed by line
//   0xC000_0000-0xCFFF_FFFF  same locations; a load is fetch-and-increment
//   0xF000_0000              write: done flag     0xF000_0004 write: result
//   0xF000_0008              read: linear tile id 0xF000_000C read: cycle count
//
// Mesh ports: index [net][dir], net 0..2 = NoC1..3, dir 0 N, 1 E, 2 S, 3 W.
module mc_tile #(parameter int XW = 1, YW = 1, X = 1, Y = 1, CORE = 0, L2_LINES = 64,
                 L1_SETS = 16, DIR_PTRS = 4) (
  input  logic clk, rst_n,
  input  logic [XW-1:0] my_x,
  input  logic [YW-1:0] my_y,
  input  logic [31:0] my_id,
  input  logic [2:0][3:0] in_v,
  input  logic [2:0][3:0][XW+YW+XW+YW+3+28+4+128-1:0] in_f,
  output logic [2:0][3:0] in_rdy,
  output logic [2:0][3:0] out_v,
  output logic [2:0][3:0][XW+YW+XW+YW+3+28+4+128-1:0] out_f,
  input  logic [2:0][3:0] out_rdy,
  output logic done,
  output logic [31:0] result,
  output logic [31:0] n_l2req
);
  localparam int FW = XW + YW + XW + YW + 3 + 28 + 4 + 128;

  // ---- core, normalised to a held request: valid until a one-cycle ready ----
  logic        req_valid, req_ready;
  logic [31:0] req_addr, req_wdata, rdata;
  logic [3:0]  req_wstrb;
  mc_core #(.CORE(CORE)) core(.clk(clk), .rst_n(rst_n), .req_valid(req_valid), .req_addr(req_addr),
    .req_wdata(req_wdata), .req_wstrb(req_wstrb), .req_ready(req_ready), .rdata(rdata));

  // ---- private RAM and registers ----
  logic [31:0] ram [1024];
  // @PROGRAM@ (bench/gen_manycore.py puts the program image here)
  logic loc_ready;
  logic [31:0] loc_rdata, cycles;
  wire is_priv = req_addr[31:28] == 4'h0;
  wire is_mmio = req_addr[31:28] == 4'hF;
  wire is_shr  = req_addr[31:28] == 4'h8 || req_addr[31:28] == 4'hC;
  // plain always: ram also has an initial block with the program image
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      loc_ready <= 1'b0;
      done <= 1'b0;
      result <= '0;
      cycles <= '0;
    end else begin
      cycles <= cycles + 1;
      loc_ready <= 1'b0;
      if (req_valid && !req_ready && !is_shr) begin
        loc_ready <= 1'b1;
        if (is_priv) begin
          loc_rdata <= ram[req_addr[11:2]];
          for (int b = 0; b < 4; b++)
            if (req_wstrb[b]) ram[req_addr[11:2]][b*8 +: 8] <= req_wdata[b*8 +: 8];
        end else begin
          loc_rdata <= req_addr[3:2] == 2'd2 ? my_id : req_addr[3:2] == 2'd3 ? cycles : 32'd0;
          if (req_wstrb != 4'd0 && req_addr[3:2] == 2'd0) done <= req_wdata[0];
          if (req_wstrb != 4'd0 && req_addr[3:2] == 2'd1) result <= req_wdata;
        end
      end
    end
  end

  // ---- L1 ----
  logic l1_ready;
  logic [31:0] l1_rdata;
  logic [2:0] lv, lr;               // local port valid/ready per network, router -> tile
  logic [2:0] tv, tr;               // tile -> router
  logic [2:0][FW-1:0] lf, tf;
  mc_l1 #(.XW(XW), .YW(YW), .X(X), .Y(Y), .SETS(L1_SETS)) l1(.clk(clk), .rst_n(rst_n),
    .my_x(my_x), .my_y(my_y),
    .c_valid(req_valid && is_shr && !req_ready), .c_amo(req_addr[30]), .c_addr(req_addr[27:0]),
    .c_wdata(req_wdata), .c_wstrb(req_wstrb), .c_ready(l1_ready), .c_rdata(l1_rdata),
    .n1_v(tv[0]), .n1_rdy(tr[0]), .n1_f(tf[0]),
    .n2_v(lv[1]), .n2_rdy(lr[1]), .n2_f(lf[1]),
    .n3_v(tv[2]), .n3_rdy(tr[2]), .n3_f(tf[2]));
  assign req_ready = loc_ready | l1_ready;
`ifdef MC_TRACE
  // +define+MC_TRACE: one line per completed core access
  always @(posedge clk)
    if (req_valid && req_ready)
      $display("%0t tile %0d %s %h %h", $time, my_id, req_wstrb != 4'd0 ? "W" : "R", req_addr,
               req_wstrb != 4'd0 ? req_wdata : rdata);
`endif
  assign rdata = loc_ready ? loc_rdata : l1_rdata;

  // ---- L2 slice ----
  mc_l2 #(.XW(XW), .YW(YW), .X(X), .Y(Y), .LINES(L2_LINES), .PTRS(DIR_PTRS)) l2(.clk(clk), .rst_n(rst_n),
    .my_x(my_x), .my_y(my_y),
    .n1_v(lv[0]), .n1_rdy(lr[0]), .n1_f(lf[0]),
    .n2_v(tv[1]), .n2_rdy(tr[1]), .n2_f(tf[1]),
    .n3_v(lv[2]), .n3_rdy(lr[2]), .n3_f(lf[2]),
    .n_req(n_l2req));

  // ---- routers: port 0 local, 1..4 = N E S W ----
  for (genvar n = 0; n < 3; n++) begin : net
    logic [4:0] iv, ir, ov, orr;
    logic [4:0][FW-1:0] ifl, ofl;
    assign iv = {in_v[n], tv[n]};
    assign ifl = {in_f[n], tf[n]};
    assign {in_rdy[n], tr[n]} = ir;
    assign {out_v[n], lv[n]} = ov;
    assign {out_f[n], lf[n]} = ofl;
    assign orr = {out_rdy[n], lr[n]};
    mc_router #(.XW(XW), .YW(YW), .FW(FW)) r(.clk(clk), .rst_n(rst_n), .my_x(my_x), .my_y(my_y),
      .in_v(iv), .in_f(ifl), .in_rdy(ir), .out_v(ov), .out_f(ofl), .out_rdy(orr));
  end
endmodule
