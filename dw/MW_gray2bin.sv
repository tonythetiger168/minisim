// MW_gray2bin - Gray code to binary: b[i] = XOR of g[WIDTH-1:i].
module MW_gray2bin #(
    parameter int WIDTH = 8
) (
    input  logic [WIDTH-1:0] g,
    output logic [WIDTH-1:0] b
);
    always_comb begin
        logic acc;
        acc = 1'b0;
        for (int i = WIDTH - 1; i >= 0; i--) begin
            acc  = acc ^ g[i];
            b[i] = acc;
        end
    end
endmodule
