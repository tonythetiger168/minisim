// MW_prod_sum - sum of NUM products: sum = a0*b0 + a1*b1 + ... (a dot
// product), each a_i A_WIDTH bits and b_i B_WIDTH bits, packed as
// {a[NUM-1], ..., a[0]}. tc = 1 treats all operands as two's complement.
// The products are exact; the sum is SUM_WIDTH bits, wrapping.
module MW_prod_sum #(
    parameter int A_WIDTH   = 8,
    parameter int B_WIDTH   = 8,
    parameter int NUM       = 4,
    parameter int SUM_WIDTH = A_WIDTH + B_WIDTH + $clog2(NUM + 1)
) (
    input  logic [NUM*A_WIDTH-1:0] a,
    input  logic [NUM*B_WIDTH-1:0] b,
    input  logic                   tc,
    output logic [SUM_WIDTH-1:0]   sum
);
    localparam int PW = A_WIDTH + B_WIDTH;
    logic [NUM*PW-1:0] p;
    logic [NUM*SUM_WIDTH-1:0] pe;
    for (genvar i = 0; i < NUM; i++) begin : g_p
        MW_mult #(.A_WIDTH(A_WIDTH), .B_WIDTH(B_WIDTH)) u_mult (
            .a(a[i*A_WIDTH +: A_WIDTH]), .b(b[i*B_WIDTH +: B_WIDTH]), .tc(tc), .product(p[i*PW +: PW]));
        // Extend each product to the sum width: sign when tc = 1.
        if (SUM_WIDTH > PW) begin : g_ext
            assign pe[i*SUM_WIDTH +: SUM_WIDTH] = {{(SUM_WIDTH-PW){tc & p[i*PW+PW-1]}}, p[i*PW +: PW]};
        end else begin : g_cut
            assign pe[i*SUM_WIDTH +: SUM_WIDTH] = p[i*PW +: SUM_WIDTH];
        end
    end
    MW_sum #(.WIDTH(SUM_WIDTH), .NUM(NUM)) u_sum (.in(pe), .sum(sum));
endmodule
