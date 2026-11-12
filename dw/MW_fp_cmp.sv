// MW_fp_cmp - IEEE 754 comparison, minimum and maximum, any format
// {sign, exponent[EXP-1:0], fraction[FRAC-1:0]}.
//   altb, aeqb, agtb: a < b, a == b, a > b (all 0 when unordered);
//   unordered = 1 when a or b is a NaN. -0 == +0.
//   z0, z1: minimumNumber and maximumNumber of IEEE 754-2019 (zctr = 0), or
//   the other way round (zctr = 1): a NaN operand loses to a number, two
//   NaNs give the canonical quiet NaN, and -0 counts as less than +0.
//   status = {4'b0, invalid}: invalid is set by a signaling NaN input (a
//   quiet comparison).
module MW_fp_cmp #(
    parameter int EXP  = 8,
    parameter int FRAC = 23
) (
    input  logic [EXP+FRAC:0] a,
    input  logic [EXP+FRAC:0] b,
    input  logic              zctr,
    output logic              altb,
    output logic              aeqb,
    output logic              agtb,
    output logic              unordered,
    output logic [EXP+FRAC:0] z0,
    output logic [EXP+FRAC:0] z1,
    output logic [4:0]        status
);
    localparam logic [EXP+FRAC:0] QNAN = {1'b0, {EXP{1'b1}}, 1'b1, {(FRAC-1){1'b0}}};
    logic [EXP+FRAC-1:0] ma, mb;
    logic sa, sb, nan_a, nan_b, snan, lt, both0;
    logic [EXP+FRAC:0] mn, mx;
    assign {sa, ma} = a;
    assign {sb, mb} = b;
    assign nan_a = &a[EXP+FRAC-1:FRAC] && |a[FRAC-1:0];
    assign nan_b = &b[EXP+FRAC-1:FRAC] && |b[FRAC-1:0];
    assign snan  = (nan_a && !a[FRAC-1]) || (nan_b && !b[FRAC-1]);
    assign both0 = (ma == 0) && (mb == 0);
    // Sign-magnitude order with -0 < +0.
    assign lt = (sa != sb) ? sa : (sa ? (ma > mb) : (ma < mb));
    assign unordered = nan_a || nan_b;
    assign aeqb = !unordered && (a == b || both0);
    assign altb = !unordered && !aeqb && lt;
    assign agtb = !unordered && !aeqb && !lt;
    always_comb begin
        if (nan_a && nan_b) begin mn = QNAN; mx = QNAN; end
        else if (nan_a)     begin mn = b; mx = b; end
        else if (nan_b)     begin mn = a; mx = a; end
        else if (lt)        begin mn = a; mx = b; end
        else                begin mn = b; mx = a; end
    end
    assign z0 = zctr ? mx : mn;
    assign z1 = zctr ? mn : mx;
    assign status = {4'b0000, snan};
endmodule
