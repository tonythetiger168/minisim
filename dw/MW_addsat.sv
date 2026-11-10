// MW_addsat - saturating adder/subtractor.
//   add_sub = 0: a + b, add_sub = 1: a - b. The result clips to the range
//   of WIDTH bits instead of wrapping: unsigned (tc = 0) to 0 .. 2^WIDTH-1,
//   signed (tc = 1) to -2^(WIDTH-1) .. 2^(WIDTH-1)-1. sat = 1 when it clipped.
module MW_addsat #(
    parameter int WIDTH = 8,
    parameter int ARCH  = 2      // as MW_add
) (
    input  logic [WIDTH-1:0] a,
    input  logic [WIDTH-1:0] b,
    input  logic             tc,
    input  logic             add_sub,
    output logic [WIDTH-1:0] sum,
    output logic             sat
);
    logic [WIDTH-1:0] s, bx;
    logic             co, ovf_s;
    assign bx = b ^ {WIDTH{add_sub}};
    MW_add #(.WIDTH(WIDTH), .ARCH(ARCH)) u_add (.a(a), .b(bx), .ci(add_sub), .sum(s), .co(co));
    // Signed overflow: the operands (after inverting b for subtract) have
    // the same sign and the sum has the other one.
    assign ovf_s = (a[WIDTH-1] == bx[WIDTH-1]) & (s[WIDTH-1] != a[WIDTH-1]);
    always_comb begin
        sum = s;
        sat = 1'b0;
        if (tc) begin
            if (ovf_s) begin
                sat = 1'b1;
                sum = {a[WIDTH-1], {(WIDTH-1){~a[WIDTH-1]}}};
            end
        end else if (co ^ add_sub) begin
            // carry out of an add, or borrow out of a subtract
            sat = 1'b1;
            sum = {WIDTH{~add_sub}};
        end
    end
endmodule
