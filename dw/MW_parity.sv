// MW_parity - parity generator and checker.
//   parity = XOR of the bits of a, inverted when ODD = 1, so that
//   {a, parity} has an even (ODD = 0) or odd (ODD = 1) number of ones.
//   error = 1 when parity_in does not match.
module MW_parity #(
    parameter int WIDTH = 8,
    parameter bit ODD   = 0
) (
    input  logic [WIDTH-1:0] a,
    input  logic             parity_in,
    output logic             parity,
    output logic             error
);
    assign parity = (^a) ^ ODD;
    assign error  = parity ^ parity_in;
endmodule
