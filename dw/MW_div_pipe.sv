// MW_div_pipe - divider with STAGES register stages: quotient, remainder
// and divide_by_0 follow a and b by STAGES clocks (as MW_div gives them).
// en = 0 holds every stage; rst_n is an asynchronous active-low reset.
// The registers sit behind the divider; a synthesis tool with register
// retiming moves them into the array to balance the stages.
module MW_div_pipe #(
    parameter int A_WIDTH = 8,
    parameter int B_WIDTH = 8,
    parameter int STAGES  = 2
) (
    input  logic               clk,
    input  logic               rst_n,
    input  logic               en,
    input  logic [A_WIDTH-1:0] a,
    input  logic [B_WIDTH-1:0] b,
    input  logic               tc,
    output logic [A_WIDTH-1:0] quotient,
    output logic [B_WIDTH-1:0] remainder,
    output logic               divide_by_0
);
    localparam int W = A_WIDTH + B_WIDTH + 1;
    logic [A_WIDTH-1:0] q;
    logic [B_WIDTH-1:0] r;
    logic               dz;
    MW_div #(.A_WIDTH(A_WIDTH), .B_WIDTH(B_WIDTH)) u_div (
        .a, .b, .tc, .quotient(q), .remainder(r), .divide_by_0(dz));
    generate
        if (STAGES == 0) begin : g_comb
            assign {quotient, remainder, divide_by_0} = {q, r, dz};
        end else begin : g_reg
            logic [W-1:0] s [STAGES];
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    for (int i = 0; i < STAGES; i++) s[i] <= '0;
                end else if (en) begin
                    s[0] <= {q, r, dz};
                    for (int i = 1; i < STAGES; i++) s[i] <= s[i-1];
                end
            end
            assign {quotient, remainder, divide_by_0} = s[STAGES-1];
        end
    endgenerate
endmodule
