// MW_fp_addsub - IEEE 754 addition and subtraction, any format
// {sign, exponent[EXP-1:0], fraction[FRAC-1:0]}, subnormals included.
//   z = a + b (op = 0) or a - b (op = 1), rounded by rnd (see MW_fp_round).
//   status = {inexact, underflow, overflow, divzero, invalid}.
//   NaN results are the canonical quiet NaN (fraction MSB set, sign 0);
//   inf - inf and a signaling NaN input raise invalid. An exact zero sum of
//   opposite signs is +0, or -0 when rounding toward -inf.
module MW_fp_addsub #(
    parameter int EXP  = 8,
    parameter int FRAC = 23
) (
    input  logic [EXP+FRAC:0] a,
    input  logic [EXP+FRAC:0] b,
    input  logic              op,
    input  logic [2:0]        rnd,
    output logic [EXP+FRAC:0] z,
    output logic [4:0]        status
);
    localparam int BIAS = (1 << (EXP - 1)) - 1;
    localparam int SW   = FRAC + 5;           // hidden bit, 3 guard bits, carry
    localparam logic [EXP+FRAC:0] QNAN = {1'b0, {EXP{1'b1}}, 1'b1, {(FRAC-1){1'b0}}};

    logic [EXP-1:0]  ea, eb;
    logic [FRAC-1:0] fa, fb;
    logic            sa, sb, nan_a, nan_b, snan, inf_a, inf_b;
    assign {sa, ea, fa} = a;
    assign sb = b[EXP+FRAC] ^ op;
    assign eb = b[EXP+FRAC-1:FRAC];
    assign fb = b[FRAC-1:0];
    assign nan_a = &ea && |fa;
    assign nan_b = &eb && |fb;
    assign inf_a = &ea && !(|fa);
    assign inf_b = &eb && !(|fb);
    assign snan  = (nan_a && !fa[FRAC-1]) || (nan_b && !fb[FRAC-1]);

    logic                 rs, rsticky;
    logic signed [31:0]   re;
    logic [SW-1:0]        rsig;
    logic [EXP+FRAC:0]    rz;
    logic [4:0]           rst;
    MW_fp_round #(.EXP(EXP), .FRAC(FRAC), .SW(SW)) u_round (
        .sign(rs), .e(re), .sig(rsig), .sticky(1'b0), .rnd, .z(rz), .status(rst));

    always_comb begin
        int xa, xb, sh;
        logic [SW-1:0] ma, mb, hi_m, lo_m, lost;
        logic s_hi, s_lo;
        xa = (ea == 0) ? 1 : int'(ea);
        xb = (eb == 0) ? 1 : int'(eb);
        ma = SW'({ea != 0, fa}) << 3;
        mb = SW'({eb != 0, fb}) << 3;
        // hi_m has the larger exponent; lo_m is shifted right to meet it,
        // the bits shifted out jammed into its least significant bit.
        if (xa >= xb) begin
            hi_m = ma; lo_m = mb; s_hi = sa; s_lo = sb; sh = xa - xb;
            re = 32'(xa - BIAS - FRAC - 3);
        end else begin
            hi_m = mb; lo_m = ma; s_hi = sb; s_lo = sa; sh = xb - xa;
            re = 32'(xb - BIAS - FRAC - 3);
        end
        if (sh > FRAC + 4) begin
            lo_m = SW'(lo_m != 0);
        end else begin
            lost  = lo_m & ~({SW{1'b1}} << sh);
            lo_m = (lo_m >> sh) | SW'(lost != 0);
        end
        if (s_hi == s_lo) begin
            rsig = hi_m + lo_m; rs = s_hi;
        end else if (hi_m >= lo_m) begin
            rsig = hi_m - lo_m; rs = s_hi;
        end else begin
            rsig = lo_m - hi_m; rs = s_lo;
        end
        if (rsig == 0) rs = (sa == sb) ? sa : (rnd == 3'd3);
        rsticky = 1'b0;
    end

    always_comb begin
        if (nan_a || nan_b || (inf_a && inf_b && sa != sb)) begin
            z = QNAN;
            status = {4'b0000, snan || !(nan_a || nan_b)};
        end else if (inf_a) begin
            z = {sa, {EXP{1'b1}}, {FRAC{1'b0}}}; status = '0;
        end else if (inf_b) begin
            z = {sb, {EXP{1'b1}}, {FRAC{1'b0}}}; status = '0;
        end else begin
            z = rz; status = rst;
        end
    end
endmodule
