// SPDX-License-Identifier: Apache-2.0
// mc_router: 5-port mesh router for bench/gen_manycore.py (one per tile and network).
// Single-flit packets, dimension-ordered XY routing, a 2-deep input FIFO per
// port and round-robin arbitration per output. Ports: 0 local, 1 north (y-1),
// 2 east (x+1), 3 south (y+1), 4 west (x-1). The flit's top bits are the
// destination {x, y}. Ready towards a neighbour depends only on FIFO counts
// (registers), so mesh links have no combinational loops.
module mc_router #(parameter int XW = 1, YW = 1, FW = 32) (
  input  logic clk, rst_n,
  input  logic [XW-1:0] my_x,
  input  logic [YW-1:0] my_y,
  input  logic [4:0] in_v,
  input  logic [4:0][FW-1:0] in_f,
  output logic [4:0] in_rdy,
  output logic [4:0] out_v,
  output logic [4:0][FW-1:0] out_f,
  input  logic [4:0] out_rdy
);
  logic [4:0][1:0][FW-1:0] q;
  logic [4:0] hd;                 // head index per input FIFO
  logic [4:0][1:0] cnt;
  logic [4:0][2:0] want;          // output wanted by each input head
  logic [4:0][2:0] rr;            // last granted input per output
  logic [4:0][2:0] gnt;           // granted input per output
  logic [4:0] pop;

  always_comb begin
    for (int i = 0; i < 5; i++) begin
      logic [FW-1:0] h;
      logic [XW-1:0] dx;
      logic [YW-1:0] dy;
      h = q[i][hd[i]];
      dx = h[FW-1 -: XW];
      dy = h[FW-1-XW -: YW];
      if (dx > my_x) want[i] = 3'd2;
      else if (dx < my_x) want[i] = 3'd4;
      else if (dy > my_y) want[i] = 3'd3;
      else if (dy < my_y) want[i] = 3'd1;
      else want[i] = 3'd0;
      in_rdy[i] = cnt[i] != 2'd2;
    end
    pop = '0;
    for (int o = 0; o < 5; o++) begin
      out_v[o] = 1'b0;
      gnt[o] = 3'd0;
      for (int k = 1; k <= 5; k++) begin
        int i;
        i = (int'(rr[o]) + k) % 5;
        if (!out_v[o] && cnt[i] != 2'd0 && want[i] == 3'(o)) begin
          out_v[o] = 1'b1;
          gnt[o] = 3'(i);
        end
      end
      out_f[o] = q[gnt[o]][hd[gnt[o]]];
      if (out_v[o] && out_rdy[o]) pop[gnt[o]] = 1'b1;
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      hd <= '0;
      cnt <= '0;
      rr <= '0;
    end else begin
      for (int i = 0; i < 5; i++) begin
        logic push;
        push = in_v[i] && in_rdy[i];
        if (push) q[i][hd[i] ^ (cnt[i] == 2'd1 || (cnt[i] == 2'd2))] <= in_f[i];
        if (pop[i]) hd[i] <= ~hd[i];
        cnt[i] <= cnt[i] + 2'(push) - 2'(pop[i]);
      end
      for (int o = 0; o < 5; o++)
        if (out_v[o] && out_rdy[o]) rr[o] <= gnt[o];
    end
  end
endmodule
