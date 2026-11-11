// Memories for the block RAM word modes: 18 bits wide (16 data and 2 parity
// bits on RAMB18E1), 9 bits wide and 2048 deep, and one read on the falling
// edge.
module ramw (
    input  wire        clk,
    input  wire        we,
    input  wire [5:0]  wa, ra,
    input  wire [17:0] wd,
    output reg  [17:0] q,
    input  wire        dwe,
    input  wire [10:0] dwa, dra,
    input  wire [8:0]  dwd,
    output reg  [8:0]  dq,
    output reg  [17:0] nq
);
    reg [17:0] mem [0:63];
    always @(posedge clk) begin
        if (we) mem[wa] <= wd;
        q <= mem[ra];
    end

    reg [8:0] deep [0:2047];
    always @(posedge clk) begin
        if (dwe) deep[dwa] <= dwd;
        dq <= deep[dra];
    end

    reg [17:0] nmem [0:63];
    always @(posedge clk) if (we) nmem[wa] <= ~wd;
    always @(negedge clk) nq <= nmem[ra];
endmodule
