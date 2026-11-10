// MW_crc - parallel CRC: DATA_WIDTH bits per clock.
//   The register starts at INIT (reset or init = 1). Each en step folds
//   in data, bit DATA_WIDTH-1 first, or bit 0 first of each byte when
//   REFLECT = 1 (the register is then kept bit-reversed, as in the
//   reflected CRCs: crc is the usual reflected value). crc = state ^ XOROUT.
//   POLY leaves out the x^WIDTH term: CRC-32 is 32'h04C11DB7, with
//   INIT and XOROUT all ones and REFLECT = 1; CRC-16/CCITT-FALSE is
//   16'h1021 with INIT 16'hFFFF.
module MW_crc #(
    parameter int               WIDTH      = 16,
    parameter int               DATA_WIDTH = 8,
    parameter logic [WIDTH-1:0] POLY       = 16'h1021,
    parameter logic [WIDTH-1:0] INIT       = '1,
    parameter logic [WIDTH-1:0] XOROUT     = '0,
    parameter bit               REFLECT    = 0
) (
    input  logic                  clk,
    input  logic                  rst_n,
    input  logic                  init,
    input  logic                  en,
    input  logic [DATA_WIDTH-1:0] data,
    output logic [WIDTH-1:0]      crc
);
    logic [WIDTH-1:0] state, nxt, rpoly;
    always_comb begin
        for (int i = 0; i < WIDTH; i++) rpoly[i] = POLY[WIDTH-1-i];
        nxt = state;
        if (REFLECT) begin
            for (int i = 0; i < DATA_WIDTH; i++)
                nxt = (nxt >> 1) ^ ((nxt[0] ^ data[i]) ? rpoly : '0);
        end else begin
            for (int i = DATA_WIDTH - 1; i >= 0; i--)
                nxt = (nxt << 1) ^ ((nxt[WIDTH-1] ^ data[i]) ? POLY : '0);
        end
    end
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)    state <= INIT;
        else if (init) state <= INIT;
        else if (en)   state <= nxt;
    end
    assign crc = state ^ XOROUT;
endmodule
