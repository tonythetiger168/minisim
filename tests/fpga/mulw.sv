// Multipliers wider than one DSP block: each is split into block-sized
// partial products and carry-chain adders.
module mulw (
    input  wire        clk,
    input  wire [31:0] a, b,
    input  wire [39:0] c,
    input  wire [32:0] d,
    output reg  [63:0] pu,      // 32 x 32 unsigned
    output reg  [43:0] ps,      // 24 x 20 signed
    output reg  [47:0] pt,      // 40 x 33 signed, truncated
    output reg  [47:0] sq       // a square, truncated
);
    always @(posedge clk) begin
        pu <= a * b;
        ps <= $signed(a[23:0]) * $signed(b[19:0]);
        pt <= $signed(c) * $signed(d);
        sq <= b * b;
    end
endmodule
