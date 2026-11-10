// MW_div - combinational divider (restoring array).
//   quotient = a / b, remainder = a % b, as the SystemVerilog operators
//   give them: tc = 0 unsigned; tc = 1 two's complement, the quotient
//   rounds toward zero and the remainder takes the sign of a.
//   b = 0 raises divide_by_0; the quotient is then the largest value of the
//   sign it would have had (all ones unsigned) and the remainder is a.
//   The one signed overflow, most negative / -1, wraps to most negative.
module MW_div #(
    parameter int A_WIDTH = 8,
    parameter int B_WIDTH = 8
) (
    input  logic [A_WIDTH-1:0] a,
    input  logic [B_WIDTH-1:0] b,
    input  logic               tc,
    output logic [A_WIDTH-1:0] quotient,
    output logic [B_WIDTH-1:0] remainder,
    output logic               divide_by_0
);
    logic [A_WIDTH-1:0] ma, q;
    logic [B_WIDTH-1:0] mb;
    logic [B_WIDTH:0]   r, d;
    logic               sa, sb;
    assign sa = tc & a[A_WIDTH-1];
    assign sb = tc & b[B_WIDTH-1];
    assign ma = sa ? (~a + 1'b1) : a;
    assign mb = sb ? (~b + 1'b1) : b;
    assign divide_by_0 = (b == '0);
    always_comb begin
        r = '0;
        q = '0;
        for (int i = A_WIDTH - 1; i >= 0; i--) begin
            r = {r[B_WIDTH-1:0], ma[i]};
            d = r - {1'b0, mb};
            if (!d[B_WIDTH]) begin
                r = d;
                q[i] = 1'b1;
            end
        end
    end
    always_comb begin
        if (divide_by_0) begin
            if (!tc)     quotient = '1;
            else if (sa) quotient = {1'b1, {(A_WIDTH-1){1'b0}}};
            else         quotient = {1'b0, {(A_WIDTH-1){1'b1}}};
            // a, cut or sign-extended to the remainder's width
            remainder = '0;
            for (int i = 0; i < B_WIDTH; i++)
                remainder[i] = (i < A_WIDTH) ? a[i] : sa;
        end else begin
            quotient  = (sa ^ sb) ? (~q + 1'b1) : q;
            remainder = sa ? (~r[B_WIDTH-1:0] + 1'b1) : r[B_WIDTH-1:0];
        end
    end
endmodule
