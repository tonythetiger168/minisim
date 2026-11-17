// SPDX-License-Identifier: Apache-2.0
// mc_l1: private L1 data cache of one tile (bench/gen_manycore.py).
// Direct-mapped, 16-byte lines, write-through and no-write-allocate, like the
// L1/L1.5 pair of a tiled many-core: reads hit locally, misses fetch the line
// from its home L2 slice over NoC1 and fill from NoC2; writes update a hit
// line and always go to the home slice, which invalidates the other sharers
// before it acknowledges. One request is outstanding per core, so the home
// slices serialise every location and the program sees sequential
// consistency. Invalidations arrive on NoC2 and are acknowledged on NoC3.
//
// Flit (MSB first): dst x | dst y | src {y, x} | type(3) | addr(28) | wstrb(4) | data(128)
module mc_l1 #(parameter int XW = 1, YW = 1, X = 1, Y = 1, SETS = 16) (
  input  logic clk, rst_n,
  input  logic [XW-1:0] my_x,
  input  logic [YW-1:0] my_y,
  // core side: shared (amo=0) or fetch-and-increment (amo=1) access
  input  logic        c_valid,
  input  logic        c_amo,
  input  logic [27:0] c_addr,
  input  logic [31:0] c_wdata,
  input  logic [3:0]  c_wstrb,
  output logic        c_ready,
  output logic [31:0] c_rdata,
  // NoC1 inject (requests), NoC2 eject (data, acks, invalidations), NoC3 inject (inv acks)
  output logic n1_v, input logic n1_rdy, output logic [XW+YW+XW+YW+3+28+4+128-1:0] n1_f,
  input  logic n2_v, output logic n2_rdy, input logic [XW+YW+XW+YW+3+28+4+128-1:0] n2_f,
  output logic n3_v, input logic n3_rdy, output logic [XW+YW+XW+YW+3+28+4+128-1:0] n3_f
);
  localparam int IW = XW + YW;
  localparam int FW = IW + IW + 3 + 28 + 4 + 128;
  localparam int N = X * Y;
  localparam int SB = $clog2(SETS);
  localparam logic [2:0] T_RD = 3'd1, T_WR = 3'd2, T_AMO = 3'd3, T_DATA = 3'd4, T_WACK = 3'd5,
                         T_INV = 3'd6, T_IACK = 3'd7;

  logic [127:0] data [SETS];
  logic [23-SB:0] tag [SETS];
  logic [SETS-1:0] valid;

  typedef enum logic {IDLE, WAIT} st_t;
  st_t st;
  logic [27:0] a;                  // latched request address

  wire [23:0] c_line = c_addr[27:4];
  wire [SB-1:0] c_set = c_line[SB-1:0];
  wire c_hit = valid[c_set] && tag[c_set] == c_line[23:SB];
  wire [31:0] c_word = data[c_set][c_addr[3:2]*32 +: 32];

  // home tile of a line: line mod N, then {x, y}
  function automatic logic [IW-1:0] home_xy(input logic [23:0] line);
    int h;
    logic [XW-1:0] hx;
    logic [YW-1:0] hy;
    h = int'(line % 24'(N));
    hx = XW'(h % X);
    hy = YW'(h / X);
    return {hx, hy};
  endfunction

  wire [2:0] n2_t = n2_f[FW-1-IW-IW -: 3];
  wire [27:0] n2_a = n2_f[FW-1-IW-IW-3 -: 28];
  wire [IW-1:0] n2_src = n2_f[FW-1-IW -: IW];   // {x, y}
  wire [23:0] n2_line = n2_a[27:4];
  wire [SB-1:0] n2_set = n2_line[SB-1:0];

  assign n2_rdy = !n3_v;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= IDLE;
      valid <= '0;
      n1_v <= 1'b0;
      n3_v <= 1'b0;
      c_ready <= 1'b0;
    end else begin
      c_ready <= 1'b0;
      if (n1_v && n1_rdy) n1_v <= 1'b0;
      if (n3_v && n3_rdy) n3_v <= 1'b0;
      case (st)
        IDLE: if (c_valid && !c_ready) begin
          a <= c_addr;
          if (!c_amo && c_wstrb == 4'd0 && c_hit) begin
            c_rdata <= c_word;
            c_ready <= 1'b1;
          end else begin
            n1_v <= 1'b1;
            n1_f <= {home_xy(c_line), my_x, my_y,
                     c_amo ? T_AMO : (c_wstrb == 4'd0 ? T_RD : T_WR),
                     c_addr, c_wstrb, 96'd0, c_wdata};
            st <= WAIT;
            if (c_amo) valid[c_set] <= 1'b0;
            else if (c_wstrb != 4'd0 && c_hit)
              for (int b = 0; b < 4; b++)
                if (c_wstrb[b]) data[c_set][c_addr[3:2]*32 + b*8 +: 8] <= c_wdata[b*8 +: 8];
          end
        end
        default: ;
      endcase
      if (n2_v && n2_rdy) begin
        case (n2_t)
          T_DATA: begin
            data[n2_set] <= n2_f[127:0];
            tag[n2_set] <= n2_line[23:SB];
            valid[n2_set] <= 1'b1;
            c_rdata <= n2_f[a[3:2]*32 +: 32];
            c_ready <= 1'b1;
            st <= IDLE;
          end
          T_WACK: begin
            c_rdata <= n2_f[31:0];
            c_ready <= 1'b1;
            st <= IDLE;
          end
          T_INV: begin
            if (valid[n2_set] && tag[n2_set] == n2_line[23:SB]) valid[n2_set] <= 1'b0;
            n3_v <= 1'b1;
            n3_f <= {n2_src, my_x, my_y, T_IACK, n2_a, 4'd0, 128'd0};
          end
          default: ;
        endcase
      end
    end
  end
endmodule
