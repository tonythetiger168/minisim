// MW_sync - multi-flop synchronizer for a signal crossing into clk's domain.
//   STAGES flops in a row (2 or more). Each bit is synchronized on its
//   own, so a multi-bit value must be Gray coded or otherwise change one
//   bit at a time.
module MW_sync #(
    parameter int WIDTH  = 1,
    parameter int STAGES = 2
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic [WIDTH-1:0] d,
    output logic [WIDTH-1:0] q
);
    logic [WIDTH-1:0] r [STAGES];
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (int i = 0; i < STAGES; i++) r[i] <= '0;
        end else begin
            r[0] <= d;
            for (int i = 1; i < STAGES; i++) r[i] <= r[i-1];
        end
    end
    assign q = r[STAGES-1];
    generate
        if (STAGES < 2) begin : g_bad
            $error("MW_sync: STAGES must be at least 2");
        end
    endgenerate
endmodule
