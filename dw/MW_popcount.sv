// MW_popcount - number of 1 bits in a.
module MW_popcount #(
    parameter int WIDTH = 8,
    localparam int CW   = $clog2(WIDTH + 1)
) (
    input  logic [WIDTH-1:0] a,
    output logic [CW-1:0]    count
);
    always_comb begin
        count = '0;
        for (int i = 0; i < WIDTH; i++) count = count + CW'(a[i]);
    end
endmodule
