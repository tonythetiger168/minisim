// MW_mult_pipe - multiplier with STAGES register stages.
//   product follows a * b by STAGES clocks. en = 0 holds every stage.
//   rst_n is an asynchronous active-low reset.
// The registers sit behind the multiplier; a synthesis tool with register
// retiming moves them into the array to balance the stages.
module MW_mult_pipe #(
    parameter int A_WIDTH = 8,
    parameter int B_WIDTH = 8,
    parameter int STAGES  = 2,
    parameter int ARCH    = 1    // as MW_mult
) (
    input  logic                       clk,
    input  logic                       rst_n,
    input  logic                       en,
    input  logic [A_WIDTH-1:0]         a,
    input  logic [B_WIDTH-1:0]         b,
    input  logic                       tc,
    output logic [A_WIDTH+B_WIDTH-1:0] product
);
    localparam int W = A_WIDTH + B_WIDTH;
    logic [W-1:0] p;
    MW_mult #(.A_WIDTH(A_WIDTH), .B_WIDTH(B_WIDTH), .ARCH(ARCH)) u_mult (
        .a(a), .b(b), .tc(tc), .product(p));
    generate
        if (STAGES == 0) begin : g_comb
            assign product = p;
        end else begin : g_reg
            logic [W-1:0] r [STAGES];
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    for (int i = 0; i < STAGES; i++) r[i] <= '0;
                end else if (en) begin
                    r[0] <= p;
                    for (int i = 1; i < STAGES; i++) r[i] <= r[i-1];
                end
            end
            assign product = r[STAGES-1];
        end
    endgenerate
endmodule
