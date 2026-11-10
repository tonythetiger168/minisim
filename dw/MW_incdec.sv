// MW_incdec - incrementer/decrementer: sum = inc_dec ? a - 1 : a + 1.
module MW_incdec #(
    parameter int WIDTH = 8
) (
    input  logic [WIDTH-1:0] a,
    input  logic             inc_dec,
    output logic [WIDTH-1:0] sum
);
    logic [WIDTH:0] c;
    assign c[0] = 1'b1;
    generate
        for (genvar i = 0; i < WIDTH; i++) begin : g_bit
            assign c[i+1] = c[i] & (a[i] ^ inc_dec);
            assign sum[i] = a[i] ^ c[i];
        end
    endgenerate
endmodule
