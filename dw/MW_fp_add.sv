// MW_fp_add - IEEE 754 z = a + b; MW_fp_addsub with op tied to 1'b0.
module MW_fp_add #(
    parameter int EXP  = 8,
    parameter int FRAC = 23
) (
    input  logic [EXP+FRAC:0] a,
    input  logic [EXP+FRAC:0] b,
    input  logic [2:0]        rnd,
    output logic [EXP+FRAC:0] z,
    output logic [4:0]        status
);
    MW_fp_addsub #(.EXP(EXP), .FRAC(FRAC)) u (.a, .b, .op(1'b0), .rnd, .z, .status);
endmodule
