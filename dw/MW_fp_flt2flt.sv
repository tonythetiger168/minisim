// MW_fp_flt2flt - IEEE 754 format conversion: a in {sign, exponent[EXP_A-1:0],
// fraction[FRAC_A-1:0]} to z in {sign, exponent[EXP_Z-1:0],
// fraction[FRAC_Z-1:0]}, rounded by rnd (see MW_fp_round) when the target is
// narrower (for example double to single, or single to half / bfloat16).
//   Infinities and zeros keep their sign; a NaN gives the target's canonical
//   quiet NaN, with invalid when it was signaling.
//   status = {inexact, underflow, overflow, divzero, invalid}.
module MW_fp_flt2flt #(
    parameter int EXP_A  = 8,
    parameter int FRAC_A = 23,
    parameter int EXP_Z  = 5,
    parameter int FRAC_Z = 10
) (
    input  logic [EXP_A+FRAC_A:0] a,
    input  logic [2:0]            rnd,
    output logic [EXP_Z+FRAC_Z:0] z,
    output logic [4:0]            status
);
    localparam int BIAS_A = (1 << (EXP_A - 1)) - 1;
    localparam logic [EXP_Z+FRAC_Z:0] QNAN = {1'b0, {EXP_Z{1'b1}}, 1'b1, {(FRAC_Z-1){1'b0}}};
    logic [EXP_A-1:0]  ea;
    logic [FRAC_A-1:0] fa;
    logic              sa;
    assign {sa, ea, fa} = a;

    logic signed [31:0]  re;
    logic [EXP_Z+FRAC_Z:0] rz;
    logic [4:0]          rst;
    assign re = 32'(((ea == 0) ? 1 : int'(ea)) - BIAS_A - FRAC_A);
    MW_fp_round #(.EXP(EXP_Z), .FRAC(FRAC_Z), .SW(FRAC_A + 1)) u_round (
        .sign(sa), .e(re), .sig({ea != 0, fa}), .sticky(1'b0), .rnd, .z(rz), .status(rst));

    always_comb begin
        if (&ea && |fa) begin
            z = QNAN; status = {4'b0000, !fa[FRAC_A-1]};
        end else if (&ea) begin
            z = {sa, {EXP_Z{1'b1}}, {FRAC_Z{1'b0}}}; status = '0;
        end else begin
            z = rz; status = rst;
        end
    end
endmodule
