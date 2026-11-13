// MW_fp_flt2i - IEEE 754 to integer conversion: z = a rounded to an integer
// by rnd (see MW_fp_round), IW bits, unsigned (tc = 0) or two's complement
// (tc = 1).
//   status = {inexact, 0, 0, 0, invalid}. A NaN, an infinity or a value
//   that rounds outside the integer range raises invalid (and not inexact)
//   and gives the nearest end of the range; a NaN gives the largest value.
module MW_fp_flt2i #(
    parameter int EXP  = 8,
    parameter int FRAC = 23,
    parameter int IW   = 32
) (
    input  logic [EXP+FRAC:0] a,
    input  logic              tc,
    input  logic [2:0]        rnd,
    output logic [IW-1:0]     z,
    output logic [4:0]        status
);
    localparam int BIAS = (1 << (EXP - 1)) - 1;
    localparam int MW   = FRAC + 1;
    localparam int LW   = IW + MW + 1;

    logic [EXP-1:0]  ea;
    logic [FRAC-1:0] fa;
    logic            sa;
    assign {sa, ea, fa} = a;

    always_comb begin
        int ex, d;
        logic [LW-1:0] sig, m, wide_lo;
        logic [2*LW-1:0] wide;
        logic guard, rest, inc, nan, inf, bad;
        logic [IW-1:0] hi, lo;
        nan = &ea && |fa;
        inf = &ea && !(|fa);
        sig = LW'({ea != 0, fa});
        ex  = ((ea == 0) ? 1 : int'(ea)) - BIAS - FRAC;
        guard = 1'b0; rest = 1'b0; bad = 1'b0;
        m = '0;
        if (nan || inf || ex >= IW) begin
            bad = 1'b1;
        end else if (ex >= 0) begin
            m = sig << ex;
        end else begin
            d = -ex;
            if (d > MW + 1) begin
                rest = sig != 0;
            end else begin
                wide  = {sig, {LW{1'b0}}} >> d;
                m     = wide[2*LW-1:LW];
                guard = wide[LW-1];
                rest  = |wide[LW-2:0];
            end
        end
        case (rnd)
            3'd0:    inc = guard & (rest | m[0]);
            3'd1:    inc = 1'b0;
            3'd2:    inc = ~sa & (guard | rest);
            3'd3:    inc = sa & (guard | rest);
            default: inc = guard;
        endcase
        m = m + LW'(inc);
        // Largest magnitude allowed for each sign, and the range ends.
        if (tc) begin
            hi = {1'b0, {(IW-1){1'b1}}};
            lo = {1'b1, {(IW-1){1'b0}}};
            if (!bad) bad = sa ? (m > LW'(lo)) : (m > LW'(hi));
        end else begin
            hi = '1;
            lo = '0;
            if (!bad) bad = sa ? (m != 0) : (m > LW'(hi));
        end
        if (bad) begin
            z = (nan || !sa) ? hi : lo;
            status = 5'b00001;
        end else begin
            z = sa ? IW'(~m + 1'b1) : IW'(m);
            status = {guard | rest, 4'b0000};
        end
    end
endmodule
