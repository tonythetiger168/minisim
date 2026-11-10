// MW_absval - absolute value of a two's complement number.
//   The most negative value has no positive counterpart and comes back
//   unchanged, as in two's complement negation.
module MW_absval #(
    parameter int WIDTH = 8
) (
    input  logic [WIDTH-1:0] a,
    output logic [WIDTH-1:0] absval
);
    logic [WIDTH-1:0] inv, neg;
    assign inv = ~a;
    MW_inc #(.WIDTH(WIDTH)) u_inc (.a(inv), .sum(neg));
    assign absval = a[WIDTH-1] ? neg : a;
endmodule
