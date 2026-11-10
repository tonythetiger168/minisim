// MW_fifo_async - dual-clock FIFO with Gray code pointers.
//   The write side (wr_clk: push, data_in, full) and the read side (rd_clk:
//   pop, data_out, empty) run on unrelated clocks. Each side sees the other
//   side's pointer through a SYNC-flop synchronizer, so full and empty are
//   safe but late: they clear a few clocks after the other side makes room
//   or adds a word. DEPTH must be a power of 2. data_out shows the oldest
//   word whenever empty = 0. A push when full or a pop when empty is ignored.
module MW_fifo_async #(
    parameter int WIDTH = 8,
    parameter int DEPTH = 16,
    parameter int SYNC  = 2,
    localparam int AW   = $clog2(DEPTH)
) (
    input  logic             wr_clk,
    input  logic             wr_rst_n,
    input  logic             push,
    input  logic [WIDTH-1:0] data_in,
    output logic             full,
    input  logic             rd_clk,
    input  logic             rd_rst_n,
    input  logic             pop,
    output logic [WIDTH-1:0] data_out,
    output logic             empty
);
    logic [WIDTH-1:0] mem [DEPTH];
    // Pointers carry one extra bit so full and empty can be told apart.
    logic [AW:0] wbin, wgray, wbin_n, wgray_n, rgray_w;
    logic [AW:0] rbin, rgray, rbin_n, rgray_n, wgray_r;

    assign wbin_n  = wbin + {{AW{1'b0}}, push & ~full};
    assign wgray_n = wbin_n ^ (wbin_n >> 1);
    assign rbin_n  = rbin + {{AW{1'b0}}, pop & ~empty};
    assign rgray_n = rbin_n ^ (rbin_n >> 1);

    always_ff @(posedge wr_clk) begin
        if (push & ~full) mem[wbin[AW-1:0]] <= data_in;
    end
    always_ff @(posedge wr_clk or negedge wr_rst_n) begin
        if (!wr_rst_n) begin
            wbin  <= '0;
            wgray <= '0;
            full  <= 1'b0;
        end else begin
            wbin  <= wbin_n;
            wgray <= wgray_n;
            // Full: the write pointer is one whole lap ahead, which in Gray
            // code is the read pointer with its top two bits inverted.
            full  <= (wgray_n == {~rgray_w[AW:AW-1], rgray_w[AW-2:0]});
        end
    end
    always_ff @(posedge rd_clk or negedge rd_rst_n) begin
        if (!rd_rst_n) begin
            rbin  <= '0;
            rgray <= '0;
            empty <= 1'b1;
        end else begin
            rbin  <= rbin_n;
            rgray <= rgray_n;
            empty <= (rgray_n == wgray_r);
        end
    end
    MW_sync #(.WIDTH(AW+1), .STAGES(SYNC)) u_r2w (.clk(wr_clk), .rst_n(wr_rst_n), .d(rgray), .q(rgray_w));
    MW_sync #(.WIDTH(AW+1), .STAGES(SYNC)) u_w2r (.clk(rd_clk), .rst_n(rd_rst_n), .d(wgray), .q(wgray_r));
    assign data_out = mem[rbin[AW-1:0]];
    generate
        if (DEPTH < 4 || (DEPTH & (DEPTH - 1)) != 0) begin : g_bad
            $error("MW_fifo_async: DEPTH must be a power of 2, 4 or more");
        end
    endgenerate
endmodule
