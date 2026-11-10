// MW_sum - sum of NUM inputs: a Wallace tree and one carry-propagate adder.
//   in = {addend[NUM-1], ..., addend[0]}, WIDTH bits each.
//   sum = total modulo 2^WIDTH. Extend the inputs before the call when the
//   full total is wanted.
module MW_sum #(
    parameter int WIDTH = 8,
    parameter int NUM   = 4,
    parameter int ARCH  = 2      // final adder, as MW_add
) (
    input  logic [NUM*WIDTH-1:0] in,
    output logic [WIDTH-1:0]     sum
);
    logic [WIDTH-1:0] s, c;
    logic             co;
    MW_csa_tree #(.WIDTH(WIDTH), .NUM(NUM)) u_tree (.in(in), .sum(s), .carry(c));
    MW_add #(.WIDTH(WIDTH), .ARCH(ARCH)) u_add (.a(s), .b(c), .ci(1'b0), .sum(sum), .co(co));
endmodule
