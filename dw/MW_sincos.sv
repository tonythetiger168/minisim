// MW_sincos - sine and cosine by CORDIC, combinational.
//   a is the angle as a fraction of a full turn (a / 2^WIDTH turns, so the
//   top two bits are the quadrant). sin and cos are signed, scaled by
//   2^(WIDTH-1) - 1 (Q1.WIDTH-1 without the -1 end): within 2 units of the
//   rounded true value for WIDTH up to 24. GUARD extra bits are kept inside.
module MW_sincos #(
    parameter int WIDTH = 16,
    parameter int GUARD = 4
) (
    input  logic        [WIDTH-1:0] a,
    output logic signed [WIDTH-1:0] sin,
    output logic signed [WIDTH-1:0] cos
);
    localparam int IW = WIDTH + GUARD + 2;          // values up to 1.65 x full scale, signed
    localparam int ZW = WIDTH + GUARD + 1;          // angles: a turn is 2^(WIDTH+GUARD)
    localparam int N  = WIDTH + GUARD;              // iterations
    localparam real PI = 3.14159265358979323846;

    // atan(2^-i) in turn units, and the CORDIC gain folded into the start
    function automatic logic signed [ZW-1:0] atan_t(input int i);
        return ZW'($rtoi($atan(2.0 ** (-i)) / (2.0 * PI) * (2.0 ** (WIDTH + GUARD)) + 0.5));
    endfunction
    function automatic logic signed [IW-1:0] x0();
        real g = 1.0;
        for (int i = 0; i < N; i++) g = g / $sqrt(1.0 + 2.0 ** (-2 * i));
        return IW'($rtoi(g * ((2.0 ** (WIDTH - 1)) - 1.0) * (2.0 ** GUARD) + 0.5));
    endfunction

    logic signed [IW-1:0] x, y, xn, c, s, cq, sq;
    logic signed [ZW-1:0] z;
    always_comb begin
        // rotate (x0, 0) by the angle within the quadrant, [0, 90) degrees
        x = x0();
        y = '0;
        z = ZW'({a[WIDTH-3:0], {GUARD{1'b0}}});
        for (int i = 0; i < N; i++) begin
            if (z >= 0) begin
                xn = x - (y >>> i);
                y  = y + (x >>> i);
                z  = z - atan_t(i);
            end else begin
                xn = x + (y >>> i);
                y  = y - (x >>> i);
                z  = z + atan_t(i);
            end
            x = xn;
        end
        case (a[WIDTH-1:WIDTH-2])
            2'd0: begin cq = x;  sq = y;  end
            2'd1: begin cq = -y; sq = x;  end
            2'd2: begin cq = -x; sq = -y; end
            default: begin cq = y; sq = -x; end
        endcase
        // drop the guard bits, rounding, and keep within the scale
        c = (cq + (IW'(1) <<< (GUARD - 1))) >>> GUARD;
        s = (sq + (IW'(1) <<< (GUARD - 1))) >>> GUARD;
        if (c > IW'((2 ** (WIDTH - 1)) - 1)) c = IW'((2 ** (WIDTH - 1)) - 1);
        if (c < -IW'((2 ** (WIDTH - 1)) - 1)) c = -IW'((2 ** (WIDTH - 1)) - 1);
        if (s > IW'((2 ** (WIDTH - 1)) - 1)) s = IW'((2 ** (WIDTH - 1)) - 1);
        if (s < -IW'((2 ** (WIDTH - 1)) - 1)) s = -IW'((2 ** (WIDTH - 1)) - 1);
        cos = c[WIDTH-1:0];
        sin = s[WIDTH-1:0];
    end
endmodule
