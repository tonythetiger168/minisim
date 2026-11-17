// SPDX-License-Identifier: Apache-2.0
// mc_l2: one slice of the distributed shared L2 / memory (bench/gen_manycore.py).
// Lines are interleaved over the tiles (home = line mod N); this slice holds
// LINES lines and a directory entry per line. The directory is limited-pointer
// with broadcast on overflow (Dir_PTRS_B): up to PTRS sharer tiles are kept,
// a further sharer sets the overflow bit and the next write invalidates every
// tile. The slice handles one request at a time:
//   RD  -> add the requester as a sharer, send the line (DATA)
//   WR  -> merge the word, invalidate the other sharers, wait for their acks, WACK;
//          the writer is then the only sharer (its write-through copy is current)
//   AMO -> fetch-and-increment the word, invalidate, wait, WACK with the old value
// NoC1 carries requests in, NoC2 carries data/acks/invalidations out, NoC3
// brings invalidation acks back and is always drained, so the three networks
// cannot deadlock each other.
module mc_l2 #(parameter int XW = 1, YW = 1, X = 1, Y = 1, LINES = 64, PTRS = 4) (
  input  logic clk, rst_n,
  input  logic [XW-1:0] my_x,
  input  logic [YW-1:0] my_y,
  input  logic n1_v, output logic n1_rdy, input logic [XW+YW+XW+YW+3+28+4+128-1:0] n1_f,
  output logic n2_v, input logic n2_rdy, output logic [XW+YW+XW+YW+3+28+4+128-1:0] n2_f,
  input  logic n3_v, output logic n3_rdy, input logic [XW+YW+XW+YW+3+28+4+128-1:0] n3_f,
  output logic [31:0] n_req                 // requests served (statistics)
);
  localparam int IW = XW + YW;
  localparam int FW = IW + IW + 3 + 28 + 4 + 128;
  localparam int N = X * Y;
  localparam int LB = $clog2(LINES);
  localparam int PB = $clog2(PTRS + 1);
  localparam logic [2:0] T_RD = 3'd1, T_WR = 3'd2, T_AMO = 3'd3, T_DATA = 3'd4, T_WACK = 3'd5,
                         T_INV = 3'd6;

  logic [127:0] mem [LINES];
  logic [PTRS-1:0][IW-1:0] ptr [LINES];
  logic [PB-1:0] cnt [LINES];
  logic [LINES-1:0] ovf;

  typedef enum logic [1:0] {IDLE, INV, ACK, RESP} st_t;
  st_t st;
  logic [IW-1:0] src;
  logic [2:0] typ;
  logic [27:0] a;
  logic [31:0] old;
  logic [PTRS-1:0][IW-1:0] tp;     // invalidation targets (pointer mode)
  logic [PB-1:0] tn;
  logic bcast;
  logic [31:0] it;                 // target iterator
  logic [XW-1:0] bx;
  logic [YW-1:0] by;
  logic [31:0] sent, acks;

  wire [IW-1:0] f_src = n1_f[FW-1-IW -: IW];
  wire [2:0] f_t = n1_f[FW-1-IW-IW -: 3];
  wire [27:0] f_a = n1_f[FW-1-IW-IW-3 -: 28];
  wire [3:0] f_be = n1_f[131:128];
  wire [31:0] f_d = n1_f[31:0];
  wire [23:0] f_line = f_a[27:4];
  wire [LB-1:0] f_idx = LB'(f_line / 24'(N));
  wire [LB-1:0] a_idx = LB'(a[27:4] / 24'(N));
  wire [31:0] f_word = mem[f_idx][f_a[3:2]*32 +: 32];

  assign n1_rdy = st == IDLE && !n2_v;
  assign n3_rdy = 1'b1;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= IDLE;
      n2_v <= 1'b0;
      ovf <= '0;
      acks <= '0;
      n_req <= '0;
      for (int l = 0; l < LINES; l++) begin
        cnt[l] <= '0;
        mem[l] <= '0;
      end
    end else begin
      if (n2_v && n2_rdy) n2_v <= 1'b0;
      if (n3_v) acks <= acks + 1;
      case (st)
        IDLE: if (n1_v && n1_rdy) begin
          n_req <= n_req + 1;
          src <= f_src;
          typ <= f_t;
          a <= f_a;
          old <= f_word;
          if (f_t == T_RD) begin
            if (!ovf[f_idx]) begin
              logic dup;
              dup = 1'b0;
              for (int p = 0; p < PTRS; p++)
                if (PB'(p) < cnt[f_idx] && ptr[f_idx][p] == f_src) dup = 1'b1;
              if (!dup) begin
                if (cnt[f_idx] == PB'(PTRS)) ovf[f_idx] <= 1'b1;
                else begin
                  ptr[f_idx][cnt[f_idx]] <= f_src;
                  cnt[f_idx] <= cnt[f_idx] + 1'b1;
                end
              end
            end
            n2_v <= 1'b1;
            n2_f <= {f_src, my_x, my_y, T_DATA, f_a, 4'd0, mem[f_idx]};
          end else begin
            if (f_t == T_AMO) mem[f_idx][f_a[3:2]*32 +: 32] <= f_word + 32'd1;
            else
              for (int b = 0; b < 4; b++)
                if (f_be[b]) mem[f_idx][f_a[3:2]*32 + b*8 +: 8] <= f_d[b*8 +: 8];
            tp <= ptr[f_idx];
            tn <= cnt[f_idx];
            bcast <= ovf[f_idx];
            // the writer keeps its (updated) copy, so it stays the one sharer;
            // an AMO requester dropped its copy when it sent the request
            if (f_t == T_WR) begin
              ptr[f_idx][0] <= f_src;
              cnt[f_idx] <= PB'(1);
            end else cnt[f_idx] <= '0;
            ovf[f_idx] <= 1'b0;
            it <= '0;
            bx <= '0;
            by <= '0;
            sent <= '0;
            acks <= n3_v ? 32'd1 : 32'd0;   // stale acks cannot exist: count from here
            st <= INV;
          end
        end
        INV: if (!n2_v || n2_rdy) begin
          // send at most one invalidation per cycle
          logic go, last;
          logic [IW-1:0] t;
          go = 1'b0;
          last = 1'b0;
          if (bcast) begin
            t = {bx, by};
            go = t != src;
            last = int'(bx) == X - 1 && int'(by) == Y - 1;
            if (int'(bx) == X - 1) begin
              bx <= '0;
              by <= by + 1'b1;
            end else bx <= bx + 1'b1;
          end else begin
            last = it + 1 >= 32'(tn);
            if (it < 32'(tn)) begin
              t = tp[it[$clog2(PTRS+1)-1:0]];
              go = t != src;
            end else t = '0;
            it <= it + 1;
          end
          n2_v <= go;
          if (go) begin
            n2_f <= {t, my_x, my_y, T_INV, a, 4'd0, 128'd0};
            sent <= sent + 1;
          end
          if (last) st <= ACK;
        end
        ACK: if (acks == sent && (!n2_v || n2_rdy)) begin
          n2_v <= 1'b1;
          n2_f <= {src, my_x, my_y, T_WACK, a, 4'd0, 96'd0, old};
          st <= RESP;
        end
        RESP: if (!n2_v || n2_rdy) st <= IDLE;
        default: ;
      endcase
    end
  end
endmodule
