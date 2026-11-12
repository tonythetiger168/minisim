// MW_exp2 - base-2 exponential of a fixed-point fraction.
//   a is 0.OP_WIDTH (all fraction bits), 0 <= a < 1. z = 2^a as
//   1.(OP_WIDTH-1): z[OP_WIDTH-1] is the integer bit, 1 <= z < 2.
//   z is 2^a * 2^(OP_WIDTH-1) rounded down, or one unit below it: the
//   error is under one unit in the last place and never upward.
//   The product of 2^(2^-i) over the 1 bits of a; the constants are
//   computed at elaboration by repeated integer square roots, rounded
//   down, with GUARD extra bits.
module MW_exp2 #(
    parameter int OP_WIDTH = 8,
    parameter int GUARD    = $clog2(OP_WIDTH) + 4
) (
    input  logic [OP_WIDTH-1:0] a,
    output logic [OP_WIDTH-1:0] z
);
    localparam int F  = OP_WIDTH + GUARD;      // fraction bits of the product
    localparam int CW = 2 * F + 4;

    function automatic logic [CW-1:0] isqrt(input logic [CW-1:0] v);
        logic [CW-1:0] r, bit_;
        r = '0;
        bit_ = CW'(1) << (CW - 2);
        while (bit_ > v) bit_ = bit_ >> 2;
        while (bit_ != 0) begin
            if (v >= r + bit_) begin
                v = v - (r + bit_);
                r = (r >> 1) + bit_;
            end else begin
                r = r >> 1;
            end
            bit_ = bit_ >> 2;
        end
        return r;
    endfunction

    // 2^(2^-k) as 1.F, rounded down.
    function automatic logic [F:0] root2(input int k);
        logic [CW-1:0] v;
        v = CW'(2) << F;
        for (int j = 0; j < k; j++) v = isqrt(v << F);
        return v[F:0];
    endfunction

    logic [F:0] p [OP_WIDTH+1];
    assign p[0] = (F+1)'(1) << F;
    for (genvar i = 0; i < OP_WIDTH; i++) begin : g_step
        localparam logic [F:0] C = root2(i + 1);    // for bit a[OP_WIDTH-1-i]
        logic [2*F+1:0] m;
        assign m = p[i] * C;
        assign p[i+1] = a[OP_WIDTH-1-i] ? m[2*F:F] : p[i];
    end
    assign z = p[OP_WIDTH][F:F-OP_WIDTH+1];
    generate
        if (OP_WIDTH < 2) begin : g_bad
            $error("MW_exp2: OP_WIDTH must be at least 2");
        end
    endgenerate
endmodule
