// MW_fp_mac - IEEE 754 fused multiply-add z = a * b + c with one rounding,
// any format {sign, exponent[EXP-1:0], fraction[FRAC-1:0]}.
//   The product is exact; it and c are aligned in a window of 3*(FRAC+1)+5
//   bits below the larger of the two, the bits of the smaller operand that
//   fall out jammed into the window's last bit, and the sum is rounded once
//   by MW_fp_round (rnd as there). status = {inexact, underflow, overflow,
//   divzero, invalid}. 0 * inf, inf - inf and a signaling NaN input give the
//   canonical quiet NaN and invalid. An exact zero sum of opposite signs is
//   +0, or -0 when rounding toward -inf.
module MW_fp_mac #(
    parameter int EXP  = 8,
    parameter int FRAC = 23
) (
    input  logic [EXP+FRAC:0] a,
    input  logic [EXP+FRAC:0] b,
    input  logic [EXP+FRAC:0] c,
    input  logic [2:0]        rnd,
    output logic [EXP+FRAC:0] z,
    output logic [4:0]        status
);
    localparam int BIAS = (1 << (EXP - 1)) - 1;
    localparam int MW   = FRAC + 1;
    localparam int PW   = 2 * MW;
    localparam int W    = 3 * MW + 5;
    localparam logic [EXP+FRAC:0] QNAN = {1'b0, {EXP{1'b1}}, 1'b1, {(FRAC-1){1'b0}}};

    logic [EXP-1:0]  ea, eb, ec;
    logic [FRAC-1:0] fa, fb, fc;
    logic            sa, sb, sc, sp;
    logic            nan_a, nan_b, nan_c, inf_a, inf_b, inf_c, zero_a, zero_b, snan;
    assign {sa, ea, fa} = a;
    assign {sb, eb, fb} = b;
    assign {sc, ec, fc} = c;
    assign sp     = sa ^ sb;
    assign nan_a  = &ea && |fa;
    assign nan_b  = &eb && |fb;
    assign nan_c  = &ec && |fc;
    assign inf_a  = &ea && !(|fa);
    assign inf_b  = &eb && !(|fb);
    assign inf_c  = &ec && !(|fc);
    assign zero_a = !(|ea) && !(|fa);
    assign zero_b = !(|eb) && !(|fb);
    assign snan   = (nan_a && !fa[FRAC-1]) || (nan_b && !fb[FRAC-1]) || (nan_c && !fc[FRAC-1]);

    logic                rs;
    logic signed [31:0]  re;
    logic [W-1:0]        rsig;
    logic [EXP+FRAC:0]   rz;
    logic [4:0]          rst;
    MW_fp_round #(.EXP(EXP), .FRAC(FRAC), .SW(W)) u_round (
        .sign(rs), .e(re), .sig(rsig), .sticky(1'b0), .rnd, .z(rz), .status(rst));

    // Place sig * 2^e in the window whose bit 0 has weight 2^el.
    function automatic logic [W-1:0] place(input logic [PW-1:0] sig, input int e, input int el);
        logic [2*W-1:0] wide;
        int s;
        s = e - el;
        if (sig == 0) return '0;
        if (s >= 0) return W'(sig) << s;
        if (-s >= W) return W'(1);        // all below the window: sticky
        wide = {W'(sig), {W{1'b0}}} >> (-s);
        return wide[2*W-1:W] | W'(|wide[W-1:0]);
    endfunction

    always_comb begin
        logic [PW-1:0] p, cs;
        int ep, ecc, tp, tc, el, pl;
        logic [W-1:0] mp, mc;
        p   = PW'({ea != 0, fa}) * PW'({eb != 0, fb});
        cs  = PW'({ec != 0, fc});
        ep  = ((ea == 0) ? 1 : int'(ea)) + ((eb == 0) ? 1 : int'(eb)) - 2 * (BIAS + FRAC);
        ecc = ((ec == 0) ? 1 : int'(ec)) - BIAS - FRAC;
        // Exponent of each operand's leading one.
        pl = -1;
        for (int i = 0; i < PW; i++) if (p[i]) pl = i;
        tp = ep + pl;
        pl = -1;
        for (int i = 0; i < PW; i++) if (cs[i]) pl = i;
        tc = ecc + pl;
        if (p == 0) tp = tc;
        if (cs == 0) tc = tp;
        el = ((tp > tc) ? tp : tc) - (W - 2);
        mp = place(p, ep, el);
        mc = place(cs, ecc, el);
        re = 32'(el);
        if (sp == sc) begin
            rsig = mp + mc; rs = sp;
        end else if (mp >= mc) begin
            rsig = mp - mc; rs = sp;
        end else begin
            rsig = mc - mp; rs = sc;
        end
        if (rsig == 0) rs = (sp == sc) ? sp : (rnd == 3'd3);
    end

    always_comb begin
        logic inf_p, bad;
        inf_p = inf_a || inf_b;
        // 0 * inf is invalid even when c is a quiet NaN; inf - inf only
        // when no operand is a NaN.
        bad = (inf_a && zero_b) || (zero_a && inf_b) ||
              (!(nan_a || nan_b || nan_c) && inf_p && inf_c && sp != sc);
        if (nan_a || nan_b || nan_c || bad) begin
            z = QNAN;
            status = {4'b0000, snan || bad};
        end else if (inf_p) begin
            z = {sp, {EXP{1'b1}}, {FRAC{1'b0}}}; status = '0;
        end else if (inf_c) begin
            z = c; status = '0;
        end else begin
            z = rz; status = rst;
        end
    end
endmodule
