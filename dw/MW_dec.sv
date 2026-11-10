// MW_dec - decrementer: sum = a - 1 (wraps).
module MW_dec #(
    parameter int WIDTH = 8
) (
    input  logic [WIDTH-1:0] a,
    output logic [WIDTH-1:0] sum
);
    // Bit i flips when every bit below it is 0.
    logic [WIDTH:0] c;
    assign c[0] = 1'b1;
    generate
        for (genvar i = 0; i < WIDTH; i++) begin : g_bit
            assign c[i+1] = c[i] & ~a[i];
            assign sum[i] = a[i] ^ c[i];
        end
    endgenerate
endmodule
