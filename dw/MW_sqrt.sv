// MW_sqrt - integer square root: root = floor(sqrt(a)), a unsigned.
//   remainder = a - root*root.
module MW_sqrt #(
    parameter int WIDTH = 8,
    localparam int RW   = (WIDTH + 1) / 2
) (
    input  logic [WIDTH-1:0] a,
    output logic [RW-1:0]    root,
    output logic [RW:0]      remainder
);
    // Digit by digit: bring down two bits of a, try subtracting
    // (root so far, 01).
    always_comb begin
        logic [2*RW-1:0] ax;
        logic [RW+1:0]   r, t;
        logic [RW-1:0]   q;
        ax = (2*RW)'(a);
        r  = '0;
        q  = '0;
        for (int i = RW - 1; i >= 0; i--) begin
            r = {r[RW-1:0], ax[2*i+1], ax[2*i]};
            t = r - {q, 2'b01};
            if (!t[RW+1]) begin
                r = t;
                q = {q[RW-2:0], 1'b1};
            end else begin
                q = {q[RW-2:0], 1'b0};
            end
        end
        root      = q;
        remainder = r[RW:0];
    end
    generate
        if (WIDTH < 3) begin : g_bad
            $error("MW_sqrt: WIDTH must be at least 3");
        end
    endgenerate
endmodule
