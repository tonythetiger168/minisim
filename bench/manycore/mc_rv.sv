// SPDX-License-Identifier: Apache-2.0
// mc_rv: a small multi-cycle RV32I core written for bench/gen_manycore.py,
// so the many-core needs no outside sources (--core mcrv). It implements all
// of RV32I except FENCE/ECALL/EBREAK/CSR (executed as no-ops) and talks to
// the tile through the same held-request port as the other cores: valid
// stays high until a one-cycle ready, read data comes with ready.
// Fetch, execute, and for loads/stores one memory access: 2 cycles plus
// memory latency per instruction, similar to PicoRV32.
module mc_rv (
  input  logic clk, rst_n,
  output logic req_valid,
  output logic [31:0] req_addr, req_wdata,
  output logic [3:0] req_wstrb,
  input  logic req_ready,
  input  logic [31:0] rdata
);
  typedef enum logic [1:0] {FETCH, EXEC, MEM} st_t;
  st_t st;
  logic [31:0] pc, ir;
  logic [31:0] x [32];

  wire [6:0] op = ir[6:0];
  wire [4:0] rd = ir[11:7], rs1 = ir[19:15], rs2 = ir[24:20];
  wire [2:0] f3 = ir[14:12];
  wire [31:0] a = rs1 == 5'd0 ? 32'd0 : x[rs1];
  wire [31:0] b = rs2 == 5'd0 ? 32'd0 : x[rs2];
  wire [31:0] imm_i = {{20{ir[31]}}, ir[31:20]};
  wire [31:0] imm_s = {{20{ir[31]}}, ir[31:25], ir[11:7]};
  wire [31:0] imm_b = {{19{ir[31]}}, ir[31], ir[7], ir[30:25], ir[11:8], 1'b0};
  wire [31:0] imm_u = {ir[31:12], 12'd0};
  wire [31:0] imm_j = {{11{ir[31]}}, ir[31], ir[19:12], ir[20], ir[30:21], 1'b0};
  wire [31:0] ea = a + (op == 7'h23 ? imm_s : imm_i);

  function automatic logic [31:0] alu(input logic [2:0] f, input logic alt, input logic [31:0] p, q);
    case (f)
      3'd0: return alt ? p - q : p + q;
      3'd1: return p << q[4:0];
      3'd2: return {31'd0, $signed(p) < $signed(q)};
      3'd3: return {31'd0, p < q};
      3'd4: return p ^ q;
      3'd5: return alt ? 32'($signed(p) >>> q[4:0]) : p >> q[4:0];
      3'd6: return p | q;
      default: return p & q;
    endcase
  endfunction

  logic take;
  always_comb
    case (f3)
      3'd0: take = a == b;
      3'd1: take = a != b;
      3'd4: take = $signed(a) < $signed(b);
      3'd5: take = $signed(a) >= $signed(b);
      3'd6: take = a < b;
      3'd7: take = a >= b;
      default: take = 1'b0;
    endcase

  // load data: pick and extend the addressed byte/half
  logic [31:0] ld;
  always_comb begin
    logic [31:0] sh;
    sh = rdata >> (8 * req_addr[1:0]);
    case (f3)
      3'd0: ld = {{24{sh[7]}}, sh[7:0]};
      3'd1: ld = {{16{sh[15]}}, sh[15:0]};
      3'd4: ld = {24'd0, sh[7:0]};
      3'd5: ld = {16'd0, sh[15:0]};
      default: ld = rdata;
    endcase
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      st <= FETCH;
      pc <= 32'd0;
      req_valid <= 1'b0;
      for (int r = 0; r < 32; r++) x[r] <= 32'd0;
    end else begin
      case (st)
        FETCH: begin
          if (!req_valid) begin
            req_valid <= 1'b1;
            req_addr <= pc;
            req_wstrb <= 4'd0;
          end else if (req_ready) begin
            req_valid <= 1'b0;
            ir <= rdata;
            st <= EXEC;
          end
        end
        EXEC: begin
          logic [31:0] res;
          logic wb;
          res = 32'd0;
          wb = 1'b1;
          pc <= pc + 4;
          st <= FETCH;
          case (op)
            7'h37: res = imm_u;                                   // LUI
            7'h17: res = pc + imm_u;                              // AUIPC
            7'h6f: begin res = pc + 4; pc <= pc + imm_j; end      // JAL
            7'h67: begin res = pc + 4; pc <= (a + imm_i) & ~32'd1; end   // JALR
            7'h63: begin wb = 1'b0; if (take) pc <= pc + imm_b; end      // branches
            7'h13: res = alu(f3, f3 == 3'd5 && ir[30], a, f3 == 3'd1 || f3 == 3'd5 ? {27'd0, ir[24:20]} : imm_i);
            7'h33: res = alu(f3, ir[30], a, b);
            7'h03, 7'h23: begin                                  // loads, stores
              wb = 1'b0;
              req_valid <= 1'b1;
              req_addr <= ea;
              req_wdata <= b << (8 * ea[1:0]);
              req_wstrb <= op == 7'h03 ? 4'd0 :
                           f3[1:0] == 2'd0 ? 4'b0001 << ea[1:0] :
                           f3[1:0] == 2'd1 ? 4'b0011 << ea[1:0] : 4'b1111;
              st <= MEM;
            end
            default: wb = 1'b0;                                   // FENCE, SYSTEM: no-ops
          endcase
          if (wb && rd != 5'd0) x[rd] <= res;
        end
        default: begin                                            // MEM
          if (req_ready) begin
            req_valid <= 1'b0;
            if (op == 7'h03 && rd != 5'd0) x[rd] <= ld;
            st <= FETCH;
          end
        end
      endcase
    end
  end
endmodule
