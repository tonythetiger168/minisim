// MW_fp_i2flt - integer to IEEE 754 conversion: z = a, rounded by rnd
// (see MW_fp_round) into {sign, exponent[EXP-1:0], fraction[FRAC-1:0]}.
//   a is IW bits, unsigned (tc = 0) or two's complement (tc = 1).
//   status = {inexact, underflow, overflow, divzero, invalid}; overflow only
//   when the format is too narrow for the integer.
module MW_fp_i2flt #(
    parameter int EXP  = 8,
    parameter int FRAC = 23,
    parameter int IW   = 32
) (
    input  logic [IW-1:0]     a,
    input  logic              tc,
    input  logic [2:0]        rnd,
    output logic [EXP+FRAC:0] z,
    output logic [4:0]        status
);
    logic          s;
    logic [IW-1:0] m;
    assign s = tc & a[IW-1];
    assign m = s ? (~a + 1'b1) : a;           // the most negative value stays right
    MW_fp_round #(.EXP(EXP), .FRAC(FRAC), .SW(IW)) u_round (
        .sign(s), .e(32'sd0), .sig(m), .sticky(1'b0), .rnd, .z, .status);
endmodule
