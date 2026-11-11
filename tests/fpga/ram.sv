// Memories for the block RAM tests: a simple dual-port RAM with a read
// enable, a FIFO (read and write of the same word in one cycle), a RAM read
// through two ports, and one read without a register, which stays flops.
module ram (
    input  wire        clk,
    input  wire        we, re,
    input  wire [5:0]  wa, ra, rb,
    input  wire [11:0] wd,
    output reg  [11:0] qa,
    output reg  [11:0] qb,
    input  wire        rst, push, pop,
    input  wire [7:0]  din,
    output reg  [7:0]  dout,
    output wire        empty,
    input  wire [3:0]  sa,
    input  wire [3:0]  sd,
    input  wire        swe,
    output wire [3:0]  sq
);
    reg [11:0] mem [0:63];
    always @(posedge clk) begin
        if (we) mem[wa] <= wd;
        if (re) qa <= mem[ra];
        qb <= mem[rb];
    end

    reg [7:0] fifo [0:127];
    reg [7:0] wp, rp;
    assign empty = wp == rp;
    always @(posedge clk) begin
        if (push) fifo[wp[6:0]] <= din;
        if (pop) dout <= fifo[rp[6:0]];
        if (rst) begin wp <= 8'd0; rp <= 8'd0; end
        else begin
            if (push) wp <= wp + 1'b1;
            if (pop) rp <= rp + 1'b1;
        end
    end

    reg [3:0] lut [0:15];
    always @(posedge clk) if (swe) lut[sa] <= sd;
    assign sq = lut[sa];
endmodule
