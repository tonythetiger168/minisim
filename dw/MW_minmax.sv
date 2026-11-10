// MW_minmax - minimum or maximum of NUM inputs packed into one vector.
//   a = {in[NUM-1], ..., in[1], in[0]}, WIDTH bits each.
//   min_max = 0 gives the minimum, 1 the maximum; tc = 1 compares signed.
//   index is the position of the chosen input (the lowest one on a tie).
module MW_minmax #(
    parameter int WIDTH = 8,
    parameter int NUM   = 2,
    localparam int IW   = (NUM > 1) ? $clog2(NUM) : 1
) (
    input  logic [NUM*WIDTH-1:0] a,
    input  logic                 tc,
    input  logic                 min_max,
    output logic [WIDTH-1:0]     value,
    output logic [IW-1:0]        index
);
    always_comb begin
        logic [WIDTH-1:0] best, cur, bx, cx;
        best  = a[WIDTH-1:0];
        index = '0;
        for (int i = 1; i < NUM; i++) begin
            cur = a[i*WIDTH +: WIDTH];
            bx = best;
            cx = cur;
            bx[WIDTH-1] = best[WIDTH-1] ^ tc;
            cx[WIDTH-1] = cur[WIDTH-1] ^ tc;
            if (min_max ? (cx > bx) : (cx < bx)) begin
                best  = cur;
                index = IW'(i);
            end
        end
        value = best;
    end
endmodule
