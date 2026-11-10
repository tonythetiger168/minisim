// MW_ram - synchronous-write memory built from flip-flops, one write port
// and one read port.
//   SYNC_READ = 0: rd_data follows rd_addr combinationally.
//   SYNC_READ = 1: rd_data is registered (rd_en loads it); a read of the
//   address being written returns the old word.
// For large memories use a compiled RAM; this one maps to DEPTH*WIDTH flops.
module MW_ram #(
    parameter int WIDTH     = 8,
    parameter int DEPTH     = 16,
    parameter bit SYNC_READ = 0,
    localparam int AW       = (DEPTH > 1) ? $clog2(DEPTH) : 1
) (
    input  logic             clk,
    input  logic             wr_en,
    input  logic [AW-1:0]    wr_addr,
    input  logic [WIDTH-1:0] wr_data,
    input  logic             rd_en,
    input  logic [AW-1:0]    rd_addr,
    output logic [WIDTH-1:0] rd_data
);
    logic [WIDTH-1:0] mem [DEPTH];
    always_ff @(posedge clk) begin
        if (wr_en) mem[wr_addr] <= wr_data;
    end
    generate
        if (SYNC_READ) begin : g_sync
            always_ff @(posedge clk) begin
                if (rd_en) rd_data <= mem[rd_addr];
            end
        end else begin : g_async
            assign rd_data = mem[rd_addr];
        end
    endgenerate
endmodule
