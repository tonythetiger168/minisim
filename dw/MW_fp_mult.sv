// MW_fp_mult - IEEE 754 multiplication z = a * b, any format
// {sign, exponent[EXP-1:0], fraction[FRAC-1:0]}, subnormals included.
//   The exact product of the significands is rounded once by MW_fp_round
//   (rnd as there). status = {inexact, underflow, overflow, divzero,
//   invalid}; 0 * inf and a signaling NaN input raise invalid and give the
//   canonical quiet NaN.
module MW_fp_mult #(
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
    localparam int SW   = 2 * FRAC + 2;
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

    logic signed [31:0] re;
    logic [SW-1:0]      rsig;
    logic [EXP+FRAC:0]  rz;
    logic [4:0]         rst;
    assign re   = 32'(((ea == 0) ? 1 : int'(ea)) + ((eb == 0) ? 1 : int'(eb)) - 2 * (BIAS + FRAC));
    assign rsig = SW'({ea != 0, fa}) * SW'({eb != 0, fb});
    MW_fp_round #(.EXP(EXP), .FRAC(FRAC), .SW(SW)) u_round (
        .sign(s), .e(re), .sig(rsig), .sticky(1'b0), .rnd, .z(rz), .status(rst));

    always_comb begin
        if (nan_a || nan_b || (inf_a && zero_b) || (zero_a && inf_b)) begin
            z = QNAN;
            status = {4'b0000, snan || !(nan_a || nan_b)};
        end else if (inf_a || inf_b) begin
            z = {s, {EXP{1'b1}}, {FRAC{1'b0}}}; status = '0;
        end else begin
            z = rz; status = rst;
        end
    end
endmodule
