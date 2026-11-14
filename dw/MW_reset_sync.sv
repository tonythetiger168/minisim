// MW_reset_sync - reset synchronizer: asserts at once, deasserts in clk's
//   domain. rst_in_n low drives rst_out_n low asynchronously; after
//   rst_in_n rises, rst_out_n rises on the STAGES-th rising edge of clk.
module MW_reset_sync #(
    parameter int STAGES = 2
) (
    input  logic clk,
    input  logic rst_in_n,
    output logic rst_out_n
);
    logic [STAGES-1:0] r;
    always_ff @(posedge clk or negedge rst_in_n) begin
        if (!rst_in_n) r <= '0;
        else           r <= {r[STAGES-2:0], 1'b1};
    end
    assign rst_out_n = r[STAGES-1];
    generate
        if (STAGES < 2) begin : g_bad
            $error("MW_reset_sync: STAGES must be at least 2");
        end
    endgenerate
endmodule
