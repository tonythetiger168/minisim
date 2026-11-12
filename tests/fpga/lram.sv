// Memories for the LUT RAM tests: a register file read through two ports
// without a clock, a 64 deep table written on the falling edge, and a small
// memory with a registered read (too small for a block RAM).
module lram (
    input  wire       clk,
    input  wire       we,
    input  wire [3:0] wa, ra, rb,
    input  wire [7:0] wd,
    output wire [7:0] qa, qb,
    input  wire       twe,
    input  wire [5:0] ta, tr,
    input  wire [4:0] td,
    output wire [4:0] tq,
    output reg  [7:0] sq
);
    reg [7:0] rf [0:15];
    always @(posedge clk) if (we) rf[wa] <= wd;
    assign qa = rf[ra];
    assign qb = rf[rb] ^ rf[ra];

    reg [4:0] tab [0:63];
    always @(negedge clk) if (twe) tab[ta] <= td;
    assign tq = tab[tr];

    reg [7:0] tiny [0:15];
    always @(posedge clk) begin
        if (we) tiny[wa] <= ~wd;
        sq <= tiny[rb];
    end
endmodule
