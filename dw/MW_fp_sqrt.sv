// MW_fp_sqrt - IEEE 754 square root z = sqrt(a), any format
// {sign, exponent[EXP-1:0], fraction[FRAC-1:0]}, subnormals included.
//   The significand is normalized, the exponent made even, and MW_sqrt takes
//   an integer root with at least FRAC + 3 bits; a nonzero remainder is the
//   sticky bit, so one rounding by MW_fp_round (rnd as there) gives the
//   correctly rounded result. sqrt(-0) = -0; a negative nonzero input or a
//   signaling NaN gives the canonical quiet NaN and invalid.
//   status = {inexact, underflow, overflow, divzero, invalid}.
module MW_fp_sqrt #(
    parameter int EXP  = 8,
    parameter int FRAC = 23
) (
    input  logic [EXP+FRAC:0] a,
    input  logic [2:0]        rnd,
    output logic [EXP+FRAC:0] z,
    output logic [4:0]        status
);
    localparam int BIAS = (1 << (EXP - 1)) - 1;
    localparam int MW   = FRAC + 1;
    localparam int K    = FRAC / 2 + 3;          // radicand gets 2K more bits
    localparam int NW   = MW + 1 + 2 * K;
    localparam int RW   = (NW + 1) / 2;
    localparam logic [EXP+FRAC:0] QNAN = {1'b0, {EXP{1'b1}}, 1'b1, {(FRAC-1){1'b0}}};

    logic [EXP-1:0]  ea;
    logic [FRAC-1:0] fa;
    logic            sa, nan_a, inf_a, zero_a;
    assign {sa, ea, fa} = a;
    assign nan_a  = &ea && |fa;
    assign inf_a  = &ea && !(|fa);
    assign zero_a = !(|ea) && !(|fa);

    logic [NW-1:0] n;
    int            ex;
    always_comb begin
        logic [MW-1:0] m;
        m  = {ea != 0, fa};
        ex = ((ea == 0) ? 1 : int'(ea)) - BIAS - FRAC;
        for (int i = 0; i < MW; i++)
            if (!m[MW-1] && m != 0) begin m = m << 1; ex--; end
        n = NW'(m) << (2 * K);
        if (ex % 2 != 0) begin n = n << 1; ex--; end
    end

    logic [RW-1:0]      root;
    logic [RW:0]        rem;
    logic signed [31:0] re;
    logic [EXP+FRAC:0]  rz;
    logic [4:0]         rst;
    MW_sqrt #(.WIDTH(NW)) u_sqrt (.a(n), .root, .remainder(rem));
    assign re = 32'((ex - 2 * K) / 2);
    MW_fp_round #(.EXP(EXP), .FRAC(FRAC), .SW(RW)) u_round (
        .sign(1'b0), .e(re), .sig(root), .sticky(rem != 0), .rnd, .z(rz), .status(rst));

    always_comb begin
        if (nan_a || (sa && !zero_a)) begin
            z = QNAN;
            status = {4'b0000, !nan_a || !fa[FRAC-1]};
        end else if (zero_a || inf_a) begin
            z = a; status = '0;
        end else begin
            z = rz; status = rst;
        end
    end
endmodule
