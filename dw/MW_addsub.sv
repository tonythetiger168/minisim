// MW_addsub - adder/subtractor.
//   add_sub = 0: sum = a + b + ci, co = carry out
//   add_sub = 1: sum = a - b - ci, co = borrow out
module MW_addsub #(
    parameter int WIDTH = 8,
    parameter int ARCH  = 2      // as MW_add
) (
    input  logic [WIDTH-1:0] a,
    input  logic [WIDTH-1:0] b,
    input  logic             ci,
    input  logic             add_sub,
    output logic [WIDTH-1:0] sum,
    output logic             co
);
    logic c;
    MW_add #(.WIDTH(WIDTH), .ARCH(ARCH)) u_add (
        .a(a), .b(b ^ {WIDTH{add_sub}}), .ci(ci ^ add_sub), .sum(sum), .co(c));
    assign co = c ^ add_sub;
endmodule
