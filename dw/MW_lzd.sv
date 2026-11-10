// MW_lzd - leading zero detector.
//   count = number of 0 bits above the highest 1 in a (WIDTH when a is 0).
//   all_0 = 1 when a is 0.
module MW_lzd #(
    parameter int WIDTH = 8,
    localparam int CW   = $clog2(WIDTH + 1)
) (
    input  logic [WIDTH-1:0] a,
    output logic [CW-1:0]    count,
    output logic             all_0
);
    always_comb begin
        logic found;
        found = 1'b0;
        count = CW'(WIDTH);
        for (int i = WIDTH - 1; i >= 0; i--) begin
            if (a[i] && !found) begin
                found = 1'b1;
                count = CW'(WIDTH - 1 - i);
            end
        end
        all_0 = ~found;
    end
endmodule
