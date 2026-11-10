// MW_mult - combinational multiplier.
//   product = a * b. tc = 0 multiplies unsigned, tc = 1 two's complement.
// ARCH:
//   0 array            one row of AND gates and one ripple adder per bit of b
//   1 Booth + Wallace  radix-4 Booth recoding halves the partial products,
//                      a Wallace tree reduces them to two rows, and a
//                      parallel prefix adder (FINAL, as MW_add ARCH) adds those
module MW_mult #(
    parameter int A_WIDTH = 8,
    parameter int B_WIDTH = 8,
    parameter int ARCH    = 1,
    parameter int FINAL   = 2
) (
    input  logic [A_WIDTH-1:0]         a,
    input  logic [B_WIDTH-1:0]         b,
    input  logic                       tc,
    output logic [A_WIDTH+B_WIDTH-1:0] product
);
    localparam int W = A_WIDTH + B_WIDTH;

    generate
        if (ARCH == 0) begin : g_array
            // Unsigned array on the magnitudes, then the sign put back.
            logic [A_WIDTH-1:0] ma;
            logic [B_WIDTH-1:0] mb;
            logic               neg;
            logic [W-1:0]       mag;
            assign ma  = (tc & a[A_WIDTH-1]) ? (~a + 1'b1) : a;
            assign mb  = (tc & b[B_WIDTH-1]) ? (~b + 1'b1) : b;
            assign neg = tc & (a[A_WIDTH-1] ^ b[B_WIDTH-1]);
            always_comb begin
                logic [W-1:0] acc;
                acc = '0;
                for (int i = 0; i < B_WIDTH; i++)
                    if (mb[i]) acc = acc + (W'(ma) << i);
                mag = acc;
            end
            assign product = neg ? (~mag + 1'b1) : mag;
        end else begin : g_booth
            // Extend both operands by their sign (or by 0) so one signed
            // Booth array serves both modes. b gets two extra bits, which
            // keeps the top digit correct for an unsigned b.
            localparam int BE  = B_WIDTH + 2;
            localparam int NPP = (BE + 1) / 2;
            logic [W-1:0]         ax;
            logic [BE:0]          bx;       // bx[0] is the implied 0 below b
            logic [NPP*W-1:0]     pp;
            logic [W-1:0]         s, c;
            logic                 co;
            assign ax = {{B_WIDTH{tc & a[A_WIDTH-1]}}, a};
            assign bx = {{2{tc & b[B_WIDTH-1]}}, b, 1'b0};
            for (genvar i = 0; i < NPP; i++) begin : g_pp
                // Digit i looks at bits 2i+1, 2i, 2i-1 of b: -2 .. +2.
                logic [2:0]   d;
                logic         one, two, neg;
                logic [W-1:0] m, sh;
                if (2*i + 2 <= BE) begin : g_full
                    assign d = bx[2*i+2 : 2*i];
                end else begin : g_top
                    assign d = {bx[BE], bx[BE : 2*i]};
                end
                assign one = d[1] ^ d[0];
                assign two = (d[2] & ~d[1] & ~d[0]) | (~d[2] & d[1] & d[0]);
                assign neg = d[2] & ~(d[1] & d[0]);
                assign m   = one ? ax : (two ? {ax[W-2:0], 1'b0} : '0);
                // Two's complement of the multiple, modulo 2^W.
                assign sh  = (neg ? (~m + 1'b1) : m) << (2*i);
                assign pp[i*W +: W] = sh;
            end
            MW_csa_tree #(.WIDTH(W), .NUM(NPP)) u_tree (.in(pp), .sum(s), .carry(c));
            MW_add #(.WIDTH(W), .ARCH(FINAL)) u_add (.a(s), .b(c), .ci(1'b0), .sum(product), .co(co));
        end
    endgenerate
endmodule
