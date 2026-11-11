// Memories for the memory table (Design::mems): a simple dual-port RAM with
// a registered read, a RAM with byte enables and an asynchronous read, and a
// register file with two write ports that is declared with a range not
// starting at zero.
module mem (
    input  logic        clk,
    input  logic        we,
    input  logic [3:0]  waddr, raddr,
    input  logic [7:0]  wdata,
    output logic [7:0]  rdata,
    input  logic [1:0]  be,
    input  logic [2:0]  baddr,
    input  logic [15:0] bdata,
    output logic [15:0] bq,
    input  logic        wa, wb,
    input  logic [2:0]  aa, ab, ra,
    input  logic [3:0]  da, db,
    output logic [3:0]  rq
);
    logic [7:0] ram [0:15];
    always_ff @(posedge clk) begin
        if (we) ram[waddr] <= wdata;
        rdata <= ram[raddr];
    end

    logic [15:0] bram [8];
    always_ff @(posedge clk) begin
        if (be[0]) bram[baddr][7:0]  <= bdata[7:0];
        if (be[1]) bram[baddr][15:8] <= bdata[15:8];
    end
    assign bq = bram[baddr];

    logic [3:0] rf [4:11];
    always_ff @(posedge clk) begin
        if (wa) rf[{1'b0, aa} + 4'd4] <= da;
        if (wb) rf[{1'b0, ab} + 4'd4] <= db;   // wins when both hit one word
    end
    assign rq = rf[{1'b0, ra} + 4'd4];
endmodule
