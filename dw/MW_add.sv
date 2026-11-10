// MW_add - adder with carry in and carry out.
//   sum = a + b + ci, co = carry out of the top bit.
// ARCH picks the carry structure; every one gives the same answer:
//   0 ripple carry        smallest, delay grows with WIDTH
//   1 carry lookahead     4-bit lookahead blocks, rippled between blocks
//   2 Kogge-Stone         parallel prefix, log2(WIDTH) levels, most wiring
//   3 Brent-Kung          parallel prefix, 2*log2(WIDTH)-1 levels, least area
//   4 carry select        BLOCK-bit blocks computed for carry 0 and 1
module MW_add #(
    parameter int WIDTH = 8,
    parameter int ARCH  = 2,
    parameter int BLOCK = 4      // block size for ARCH 4
) (
    input  logic [WIDTH-1:0] a,
    input  logic [WIDTH-1:0] b,
    input  logic             ci,
    output logic [WIDTH-1:0] sum,
    output logic             co
);
    logic [WIDTH-1:0] g, p;      // generate, propagate per bit
    logic [WIDTH:0]   c;         // c[i] = carry into bit i
    assign g = a & b;
    assign p = a ^ b;
    assign c[0] = ci;
    assign sum = p ^ c[WIDTH-1:0];
    assign co  = c[WIDTH];

    generate
        if (ARCH == 0) begin : g_ripple
            for (genvar i = 0; i < WIDTH; i++) begin : g_bit
                assign c[i+1] = g[i] | (p[i] & c[i]);
            end
        end else if (ARCH == 1) begin : g_cla
            for (genvar i = 0; i < WIDTH; i += 4) begin : g_blk
                localparam int N = (WIDTH - i < 4) ? WIDTH - i : 4;
                // Each carry in the block is a two-level function of the
                // block's carry in.
                for (genvar j = 0; j < N; j++) begin : g_c
                    logic [j:0] term;
                    for (genvar k = 0; k <= j; k++) begin : g_t
                        // g[i+k] propagated through bits k+1..j
                        if (k == j) begin : g_last
                            assign term[k] = g[i+k];
                        end else begin : g_mid
                            assign term[k] = g[i+k] & (&p[i+j:i+k+1]);
                        end
                    end
                    assign c[i+j+1] = (|term) | ((&p[i+j:i]) & c[i]);
                end
            end
        end else if (ARCH == 2) begin : g_ks
            localparam int L = (WIDTH > 1) ? $clog2(WIDTH) : 1;
            logic [WIDTH-1:0] gg [0:L];
            logic [WIDTH-1:0] pp [0:L];
            assign gg[0] = g;
            assign pp[0] = p;
            for (genvar l = 0; l < L; l++) begin : g_lvl
                for (genvar i = 0; i < WIDTH; i++) begin : g_bit
                    if (i >= (1 << l)) begin : g_op
                        assign gg[l+1][i] = gg[l][i] | (pp[l][i] & gg[l][i-(1<<l)]);
                        assign pp[l+1][i] = pp[l][i] & pp[l][i-(1<<l)];
                    end else begin : g_buf
                        assign gg[l+1][i] = gg[l][i];
                        assign pp[l+1][i] = pp[l][i];
                    end
                end
            end
            for (genvar i = 0; i < WIDTH; i++) begin : g_c
                assign c[i+1] = gg[L][i] | (pp[L][i] & ci);
            end
        end else if (ARCH == 3) begin : g_bk
            localparam int L = (WIDTH > 1) ? $clog2(WIDTH) : 1;
            // Up sweep: level l combines spans of 2^l at positions
            // i % 2^(l+1) == 2^(l+1)-1. Down sweep fills in the rest.
            logic [WIDTH-1:0] gu [0:L];
            logic [WIDTH-1:0] pu [0:L];
            logic [WIDTH-1:0] gd [0:L];
            logic [WIDTH-1:0] pd [0:L];
            assign gu[0] = g;
            assign pu[0] = p;
            for (genvar l = 0; l < L; l++) begin : g_up
                for (genvar i = 0; i < WIDTH; i++) begin : g_bit
                    if ((i % (1 << (l+1))) == (1 << (l+1)) - 1) begin : g_op
                        assign gu[l+1][i] = gu[l][i] | (pu[l][i] & gu[l][i-(1<<l)]);
                        assign pu[l+1][i] = pu[l][i] & pu[l][i-(1<<l)];
                    end else begin : g_buf
                        assign gu[l+1][i] = gu[l][i];
                        assign pu[l+1][i] = pu[l][i];
                    end
                end
            end
            assign gd[L] = gu[L];
            assign pd[L] = pu[L];
            for (genvar l = L - 1; l >= 0; l--) begin : g_dn
                for (genvar i = 0; i < WIDTH; i++) begin : g_bit
                    // Positions i % 2^(l+1) == 2^l - 1, above the first span,
                    // take the finished prefix 2^l below them.
                    if ((i % (1 << (l+1))) == (1 << l) - 1 && i >= (1 << (l+1)) - 1) begin : g_op
                        assign gd[l][i] = gd[l+1][i] | (pd[l+1][i] & gd[l+1][i-(1<<l)]);
                        assign pd[l][i] = pd[l+1][i] & pd[l+1][i-(1<<l)];
                    end else begin : g_buf
                        assign gd[l][i] = gd[l+1][i];
                        assign pd[l][i] = pd[l+1][i];
                    end
                end
            end
            for (genvar i = 0; i < WIDTH; i++) begin : g_c
                assign c[i+1] = gd[0][i] | (pd[0][i] & ci);
            end
        end else begin : g_csel
            for (genvar i = 0; i < WIDTH; i += BLOCK) begin : g_blk
                localparam int N = (WIDTH - i < BLOCK) ? WIDTH - i : BLOCK;
                logic [N:0] c0, c1;
                assign c0[0] = 1'b0;
                assign c1[0] = 1'b1;
                for (genvar j = 0; j < N; j++) begin : g_bit
                    assign c0[j+1] = g[i+j] | (p[i+j] & c0[j]);
                    assign c1[j+1] = g[i+j] | (p[i+j] & c1[j]);
                    assign c[i+j+1] = c[i] ? c1[j+1] : c0[j+1];
                end
            end
        end
    endgenerate
endmodule
