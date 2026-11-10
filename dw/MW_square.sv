// MW_square - square = a * a. tc = 1 treats a as two's complement.
module MW_square #(
    parameter int WIDTH = 8,
    parameter int ARCH  = 1      // as MW_mult
) (
    input  logic [WIDTH-1:0]   a,
    input  logic               tc,
    output logic [2*WIDTH-1:0] square
);
    MW_mult #(.A_WIDTH(WIDTH), .B_WIDTH(WIDTH), .ARCH(ARCH)) u_mult (
        .a(a), .b(a), .tc(tc), .product(square));
endmodule
