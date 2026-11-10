// MW_mult_seq - sequential shift-and-add multiplier, one bit of b per clock.
//   Pulse start with a, b and tc valid. done rises B_WIDTH clocks later and
//   stays up, with product held, until the next start.
module MW_mult_seq #(
    parameter int A_WIDTH = 8,
    parameter int B_WIDTH = 8
) (
    input  logic                       clk,
    input  logic                       rst_n,
    input  logic                       start,
    input  logic [A_WIDTH-1:0]         a,
    input  logic [B_WIDTH-1:0]         b,
    input  logic                       tc,
    output logic                       done,
    output logic [A_WIDTH+B_WIDTH-1:0] product
);
    localparam int W  = A_WIDTH + B_WIDTH;
    localparam int CW = $clog2(B_WIDTH + 1);
    logic [A_WIDTH-1:0] ma;
    logic [B_WIDTH-1:0] mb;
    logic [W-1:0]       acc;       // {partial sum, multiplier bits still to do}
    logic [A_WIDTH:0]   add;
    logic [CW-1:0]      cnt;
    logic               busy, neg;

    assign mb  = (tc & b[B_WIDTH-1]) ? (~b + 1'b1) : b;
    assign add = {1'b0, acc[W-1:B_WIDTH]} + (acc[0] ? {1'b0, ma} : '0);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            ma <= '0; acc <= '0; cnt <= '0; busy <= 1'b0; done <= 1'b0; neg <= 1'b0;
        end else if (start) begin
            ma   <= (tc & a[A_WIDTH-1]) ? (~a + 1'b1) : a;
            acc  <= {{A_WIDTH{1'b0}}, mb};
            neg  <= tc & (a[A_WIDTH-1] ^ b[B_WIDTH-1]);
            cnt  <= CW'(B_WIDTH);
            busy <= 1'b1;
            done <= 1'b0;
        end else if (busy) begin
            acc <= {add, acc[B_WIDTH-1:1]};
            cnt <= cnt - 1'b1;
            if (cnt == CW'(1)) begin
                busy <= 1'b0;
                done <= 1'b1;
            end
        end
    end
    assign product = neg ? (~acc + 1'b1) : acc;
    generate
        if (B_WIDTH < 2) begin : g_bad
            $error("MW_mult_seq: B_WIDTH must be at least 2");
        end
    endgenerate
endmodule
