// MW_fp_round - rounds and packs a value into an IEEE 754 binary format
// {sign, exponent[EXP-1:0], fraction[FRAC-1:0]}; the building block of the
// other MW_fp_* modules.
//   The value is (-1)^sign * sig * 2^e, plus something smaller than one unit
//   of sig's least significant bit when sticky is 1. sig = 0 with sticky = 0
//   gives a zero of the given sign.
//   rnd: 0 nearest-even, 1 toward zero, 2 toward +inf, 3 toward -inf,
//        4 nearest, ties away from zero.
//   status = {inexact, underflow, overflow, divzero (always 0), invalid
//   (always 0)}. Underflow is tininess before rounding and inexact.
//   e is a signed EW-bit exponent; EW = 32 covers every format up to EXP = 24.
module MW_fp_round #(
    parameter int EXP  = 8,
    parameter int FRAC = 23,
    parameter int SW   = FRAC + 4,
    parameter int EW   = 32
) (
    input  logic                 sign,
    input  logic signed [EW-1:0] e,
    input  logic [SW-1:0]        sig,
    input  logic                 sticky,
    input  logic [2:0]           rnd,
    output logic [EXP+FRAC:0]    z,
    output logic [4:0]           status
);
    localparam int BIAS = (1 << (EXP - 1)) - 1;
    localparam int EMAX = (1 << EXP) - 1;     // the all-ones exponent field
    localparam int RW   = EXP + FRAC + 2;     // result width with a carry bit

    always_comb begin
        int p, d;
        logic signed [EW+1:0] eb;
        logic [RW-1:0] kept, base, r;
        logic [2*SW-1:0] wide;
        logic guard, rest, inc, inexact, tiny, ovf, to_inf;
        // Leading one of sig.
        p = -1;
        for (int i = 0; i < SW; i++)
            if (sig[i]) p = i;
        eb = (EW+2)'(e) + p + BIAS;           // biased exponent if normal
        tiny = (p >= 0) && (eb < 1);
        d = p - FRAC;                          // bits to drop
        if (eb < 1) d = d + int'(1 - eb);      // subnormal: fixed exponent
        if (p < 0) begin
            kept = '0; guard = 1'b0; rest = sticky;
        end else if (d <= 0) begin
            kept = RW'(sig) << (-d);
            guard = 1'b0; rest = sticky;
        end else if (d > SW) begin
            kept = '0; guard = 1'b0; rest = 1'b1;
        end else begin
            wide  = {sig, {SW{1'b0}}} >> d;
            kept  = RW'(wide[2*SW-1:SW]);
            guard = wide[SW-1];
            rest  = (|wide[SW-2:0]) | sticky;
        end
        inexact = guard | rest;
        case (rnd)
            3'd0:    inc = guard & (rest | kept[0]);
            3'd1:    inc = 1'b0;
            3'd2:    inc = ~sign & inexact;
            3'd3:    inc = sign & inexact;
            default: inc = guard;
        endcase
        // Normal: (eb-1) << FRAC plus 1.fraction carries into the exponent;
        // subnormal: the fraction alone, rounding up into the smallest normal.
        base = (p >= 0 && eb > 1) ? RW'(eb - 1) << FRAC : '0;
        r = base + kept + RW'(inc);
        ovf = (p >= 0) && (eb >= EMAX || r[RW-1:FRAC] >= RW'(EMAX));
        case (rnd)
            3'd1:    to_inf = 1'b0;
            3'd2:    to_inf = ~sign;
            3'd3:    to_inf = sign;
            default: to_inf = 1'b1;
        endcase
        if (ovf) begin
            z = to_inf ? {sign, {EXP{1'b1}}, {FRAC{1'b0}}}
                       : {sign, {(EXP-1){1'b1}}, 1'b0, {FRAC{1'b1}}};
            status = 5'b10100;
        end else begin
            z = {sign, r[EXP+FRAC-1:0]};
            status = {inexact, tiny & inexact, 3'b000};
        end
    end
    generate
        if (EXP < 2 || FRAC < 1) begin : g_bad
            $error("MW_fp_round: need EXP >= 2 and FRAC >= 1");
        end
    endgenerate
endmodule
