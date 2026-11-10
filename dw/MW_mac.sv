// MW_mac - multiply-accumulate: mac = a * b + c.
//   tc = 1 treats a, b and c as two's complement. c is A_WIDTH+B_WIDTH bits
//   and the result wraps to that width.
// The addend joins the Wallace tree as one more row, so the whole
// operation costs one carry-propagate adder.
module MW_mac #(
    parameter int A_WIDTH = 8,
    parameter int B_WIDTH = 8,
    parameter int FINAL   = 2    // final adder, as MW_add ARCH
) (
    input  logic [A_WIDTH-1:0]         a,
    input  logic [B_WIDTH-1:0]         b,
    input  logic [A_WIDTH+B_WIDTH-1:0] c,
    input  logic                       tc,
    output logic [A_WIDTH+B_WIDTH-1:0] mac
);
    localparam int W   = A_WIDTH + B_WIDTH;
    localparam int BE  = B_WIDTH + 2;
    localparam int NPP = (BE + 1) / 2;
    logic [W-1:0]           ax;
    logic [BE:0]            bx;
    logic [(NPP+1)*W-1:0]   pp;
    logic [W-1:0]           s, cy;
    logic                   co;
    assign ax = {{B_WIDTH{tc & a[A_WIDTH-1]}}, a};
    assign bx = {{2{tc & b[B_WIDTH-1]}}, b, 1'b0};
    generate
        for (genvar i = 0; i < NPP; i++) begin : g_pp
            logic [2:0]   d;
            logic         one, two, neg;
            logic [W-1:0] m;
            if (2*i + 2 <= BE) begin : g_full
                assign d = bx[2*i+2 : 2*i];
            end else begin : g_top
                assign d = {bx[BE], bx[BE : 2*i]};
            end
            assign one = d[1] ^ d[0];
            assign two = (d[2] & ~d[1] & ~d[0]) | (~d[2] & d[1] & d[0]);
            assign neg = d[2] & ~(d[1] & d[0]);
            assign m   = one ? ax : (two ? {ax[W-2:0], 1'b0} : '0);
            assign pp[i*W +: W] = (neg ? (~m + 1'b1) : m) << (2*i);
        end
    endgenerate
    assign pp[NPP*W +: W] = c;
    MW_csa_tree #(.WIDTH(W), .NUM(NPP + 1)) u_tree (.in(pp), .sum(s), .carry(cy));
    MW_add #(.WIDTH(W), .ARCH(FINAL)) u_add (.a(s), .b(cy), .ci(1'b0), .sum(mac), .co(co));
endmodule
