// MW_decode - binary to one-hot decoder: bit a of out is 1 when en = 1.
module MW_decode #(
    parameter int WIDTH = 3,
    localparam int N    = 1 << WIDTH
) (
    input  logic [WIDTH-1:0] a,
    input  logic             en,
    output logic [N-1:0]     out
);
    always_comb begin
        for (int i = 0; i < N; i++) out[i] = en && (a == WIDTH'(i));
    end
endmodule
