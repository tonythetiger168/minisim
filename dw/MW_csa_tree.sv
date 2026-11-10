// MW_csa_tree - Wallace tree: reduces NUM addends to two.
//   in = {addend[NUM-1], ..., addend[0]}, WIDTH bits each.
//   sum + carry = the total of the addends, modulo 2^WIDTH.
// Each level takes the rows three at a time through 3:2 compressors, so
// the depth is about log1.5(NUM) full adders whatever the width.
module MW_csa_tree #(
    parameter int WIDTH = 8,
    parameter int NUM   = 4
) (
    input  logic [NUM*WIDTH-1:0] in,
    output logic [WIDTH-1:0]     sum,
    output logic [WIDTH-1:0]     carry
);
    always_comb begin
        logic [WIDTH-1:0] v [NUM+2];
        logic [WIDTH-1:0] t [NUM+2];
        logic [WIDTH-1:0] x, y, z, m;
        int n, k, i;
        for (int j = 0; j < NUM + 2; j++) begin
            v[j] = '0;
            t[j] = '0;
        end
        for (int j = 0; j < NUM; j++) v[j] = in[j*WIDTH +: WIDTH];
        n = NUM;
        // NUM levels is more than any tree needs; the body does nothing
        // once two rows are left.
        for (int lvl = 0; lvl < NUM; lvl++) begin
            if (n > 2) begin
                k = 0;
                i = 0;
                for (int j = 0; j < NUM; j += 3) begin
                    if (j + 2 < n) begin
                        x = v[j];
                        y = v[j+1];
                        z = v[j+2];
                        m = (x & y) | (x & z) | (y & z);
                        t[k]   = x ^ y ^ z;
                        t[k+1] = m << 1;
                        k = k + 2;
                        i = j + 3;
                    end
                end
                for (int j = 0; j < NUM; j++) begin
                    if (j >= i && j < n) begin
                        t[k] = v[j];
                        k = k + 1;
                    end
                end
                for (int j = 0; j < NUM + 2; j++) v[j] = (j < k) ? t[j] : '0;
                n = k;
            end
        end
        sum   = v[0];
        carry = (NUM > 1) ? v[1] : '0;
    end
endmodule
