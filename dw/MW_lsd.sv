// MW_lsd - leading sign detector for a two's complement number.
//   count = number of bits below the sign bit that repeat it, which is how
//   far a can shift left without changing value (WIDTH-1 for 0 and -1).
module MW_lsd #(
    parameter int WIDTH = 8,
    localparam int CW   = (WIDTH > 1) ? $clog2(WIDTH) : 1
) (
    input  logic [WIDTH-1:0] a,
    output logic [CW-1:0]    count
);
    always_comb begin
        logic found;
        found = 1'b0;
        count = CW'(WIDTH - 1);
        for (int i = WIDTH - 2; i >= 0; i--) begin
            if (a[i] != a[WIDTH-1] && !found) begin
                found = 1'b1;
                count = CW'(WIDTH - 2 - i);
            end
        end
    end
endmodule
