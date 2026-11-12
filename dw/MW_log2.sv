// MW_log2 - base-2 logarithm of a normalized fixed-point number.
//   a is 1.(OP_WIDTH-1): a[OP_WIDTH-1] is the integer bit and must be 1, so
//   1 <= a < 2. z = log2(a) as 0.OP_WIDTH (all fraction bits), 0 <= z < 1.
//   z is log2(a) * 2^OP_WIDTH rounded down, or one unit below it: the
//   error is under one unit in the last place and never upward.
//   Digit by digit: square, and a square of 2 or more gives a 1 bit and is
//   halved. GUARD extra bits keep the truncation error under one unit.
//   An a without its top bit set gives z = 0.
module MW_log2 #(
    parameter int OP_WIDTH = 8,
    parameter int GUARD    = $clog2(OP_WIDTH) + 3
) (
    input  logic [OP_WIDTH-1:0] a,
    output logic [OP_WIDTH-1:0] z
);
    localparam int F = OP_WIDTH - 1 + GUARD;   // fraction bits of y
    always_comb begin
        logic [F:0]       y;                   // 1.F, 1 <= y < 2
        logic [2*F+1:0]   sq;                  // 2.(2F)
        y = {a, {GUARD{1'b0}}};
        z = '0;
        for (int i = OP_WIDTH - 1; i >= 0; i--) begin
            sq = y * y;
            if (sq[2*F+1]) begin               // y^2 >= 2: halve it
                z[i] = 1'b1;
                y = sq[2*F+1:F+1];
            end else begin
                y = sq[2*F:F];
            end
        end
        if (!a[OP_WIDTH-1]) z = '0;
    end
    generate
        if (OP_WIDTH < 2) begin : g_bad
            $error("MW_log2: OP_WIDTH must be at least 2");
        end
    endgenerate
endmodule
