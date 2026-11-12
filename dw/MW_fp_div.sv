// MW_fp_div - IEEE 754 division z = a / b, any format
// {sign, exponent[EXP-1:0], fraction[FRAC-1:0]}, subnormals included.
//   Both significands are normalized, the quotient is taken to FRAC + 3
//   bits with MW_div and the remainder becomes the sticky bit, so one
//   rounding by MW_fp_round (rnd as there) gives the correctly rounded
//   result. status = {inexact, underflow, overflow, divzero, invalid}:
//   finite / 0 gives a signed infinity and divzero; 0 / 0, inf / inf and a
//   signaling NaN input give the canonical quiet NaN and invalid.
module MW_fp_div #(
    parameter int EXP  = 8,
    parameter int FRAC = 23
) (
    input  logic [EXP+FRAC:0] a,
    input  logic [EXP+FRAC:0] b,
    input  logic [2:0]        rnd,
    output logic [EXP+FRAC:0] z,
    output logic [4:0]        status
);
    localparam int BIAS = (1 << (EXP - 1)) - 1;
    localparam int MW   = FRAC + 1;               // significand width
    localparam int QW   = 2 * FRAC + 4;           // dividend sig << (FRAC + 3)
    localparam logic [EXP+FRAC:0] QNAN = {1'b0, {EXP{1'b1}}, 1'b1, {(FRAC-1){1'b0}}};

    logic [EXP-1:0]  ea, eb;
    logic [FRAC-1:0] fa, fb;
    logic            sa, sb, s, nan_a, nan_b, snan, inf_a, inf_b, zero_a, zero_b;
    assign {sa, ea, fa} = a;
    assign {sb, eb, fb} = b;
    assign s      = sa ^ sb;
    assign nan_a  = &ea && |fa;
    assign nan_b  = &eb && |fb;
    assign inf_a  = &ea && !(|fa);
    assign inf_b  = &eb && !(|fb);
    assign zero_a = !(|ea) && !(|fa);
    assign zero_b = !(|eb) && !(|fb);
    assign snan   = (nan_a && !fa[FRAC-1]) || (nan_b && !fb[FRAC-1]);

    // Normalize: shift each significand until its leading one is bit FRAC.
    logic [MW-1:0] na, nb;
    int            la, lb;
    always_comb begin
        na = {ea != 0, fa};
        nb = {eb != 0, fb};
        la = 0; lb = 0;
        for (int i = 0; i < MW; i++) begin
            if (!na[MW-1] && na != 0) begin na = na << 1; la++; end
            if (!nb[MW-1] && nb != 0) begin nb = nb << 1; lb++; end
        end
    end

    logic [QW-1:0]      q;
    logic [MW-1:0]      rem;
    logic               dz;
    MW_div #(.A_WIDTH(QW), .B_WIDTH(MW)) u_div (
        .a({na, {(FRAC + 3){1'b0}}}), .b(nb), .tc(1'b0),
        .quotient(q), .remainder(rem), .divide_by_0(dz));

    logic signed [31:0] re;
    logic [EXP+FRAC:0]  rz;
    logic [4:0]         rst;
    assign re = 32'(((ea == 0) ? 1 : int'(ea)) - la - ((eb == 0) ? 1 : int'(eb)) + lb
                    - (FRAC + 3));
    MW_fp_round #(.EXP(EXP), .FRAC(FRAC), .SW(QW)) u_round (
        .sign(s), .e(re), .sig(q), .sticky(rem != 0), .rnd, .z(rz), .status(rst));

    always_comb begin
        if (nan_a || nan_b || (inf_a && inf_b) || (zero_a && zero_b)) begin
            z = QNAN;
            status = {4'b0000, snan || !(nan_a || nan_b)};
        end else if (inf_a) begin
            z = {s, {EXP{1'b1}}, {FRAC{1'b0}}}; status = '0;
        end else if (zero_b) begin
            z = {s, {EXP{1'b1}}, {FRAC{1'b0}}}; status = 5'b00010;
        end else if (inf_b || zero_a) begin
            z = {s, {(EXP + FRAC){1'b0}}}; status = '0;
        end else begin
            z = rz; status = rst;
        end
    end
endmodule
