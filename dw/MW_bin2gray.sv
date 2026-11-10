// MW_bin2gray - binary to Gray code.
module MW_bin2gray #(
    parameter int WIDTH = 8
) (
    input  logic [WIDTH-1:0] b,
    output logic [WIDTH-1:0] g
);
    assign g = b ^ (b >> 1);
endmodule
