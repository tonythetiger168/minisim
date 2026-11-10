// MW_shifter - barrel shifter.
//   dir = 0 shifts left, dir = 1 shifts right by sh places.
//   arith = 1 makes a right shift fill with the sign bit; otherwise zeros
//   come in. A shift of WIDTH or more gives all fill bits.
module MW_shifter #(
    parameter int WIDTH = 8,
    parameter int SH_WIDTH = (WIDTH > 1) ? $clog2(WIDTH) : 1
) (
    input  logic [WIDTH-1:0]    data,
    input  logic [SH_WIDTH-1:0] sh,
    input  logic                dir,
    input  logic                arith,
    output logic [WIDTH-1:0]    out
);
    // One mux row per bit of sh. A left shift runs through the same rows
    // with the word reversed going in and coming out.
    always_comb begin
        logic [WIDTH-1:0] v, n;
        logic             fill;
        fill = dir & arith & data[WIDTH-1];
        out = '0;
        n   = '0;
        for (int i = 0; i < WIDTH; i++) v[i] = dir ? data[i] : data[WIDTH-1-i];
        for (int s = 0; s < SH_WIDTH; s++) begin
            if (sh[s]) begin
                for (int i = 0; i < WIDTH; i++) begin
                    // 1 << s can pass WIDTH (and int range) for a wide sh
                    if (s < 31 && i + (1 << s) < WIDTH) n[i] = v[i + (1 << s)];
                    else                                 n[i] = fill;
                end
                v = n;
            end
        end
        for (int i = 0; i < WIDTH; i++) out[i] = dir ? v[i] : v[WIDTH-1-i];
    end
endmodule
