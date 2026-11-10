// MW_rotate - barrel rotator.
//   dir = 0 rotates left, dir = 1 rotates right by sh places (modulo WIDTH).
module MW_rotate #(
    parameter int WIDTH = 8,
    parameter int SH_WIDTH = (WIDTH > 1) ? $clog2(WIDTH) : 1
) (
    input  logic [WIDTH-1:0]    data,
    input  logic [SH_WIDTH-1:0] sh,
    input  logic                dir,
    output logic [WIDTH-1:0]    out
);
    always_comb begin
        logic [WIDTH-1:0] v, n;
        int               amt;
        out = '0;
        n   = '0;
        for (int i = 0; i < WIDTH; i++) v[i] = dir ? data[i] : data[WIDTH-1-i];
        amt = 1 % WIDTH;                 // 2^s modulo WIDTH for row s
        for (int s = 0; s < SH_WIDTH; s++) begin
            if (sh[s]) begin
                for (int i = 0; i < WIDTH; i++) n[i] = v[(i + amt) % WIDTH];
                v = n;
            end
            amt = (amt * 2) % WIDTH;
        end
        for (int i = 0; i < WIDTH; i++) out[i] = dir ? v[i] : v[WIDTH-1-i];
    end
endmodule
