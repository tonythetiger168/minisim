// MW_sub - subtractor with borrow in and borrow out.
//   diff = a - b - bi, bo = 1 when a < b + bi (unsigned).
module MW_sub #(
    parameter int WIDTH = 8,
    parameter int ARCH  = 2      // as MW_add
) (
    input  logic [WIDTH-1:0] a,
    input  logic [WIDTH-1:0] b,
    input  logic             bi,
    output logic [WIDTH-1:0] diff,
    output logic             bo
);
    logic co;
    MW_add #(.WIDTH(WIDTH), .ARCH(ARCH)) u_add (.a(a), .b(~b), .ci(~bi), .sum(diff), .co(co));
    assign bo = ~co;
endmodule
