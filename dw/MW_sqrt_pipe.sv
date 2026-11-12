// MW_sqrt_pipe - integer square root with STAGES register stages: root and
// remainder follow a by STAGES clocks (as MW_sqrt gives them). en = 0
// holds every stage; rst_n is an asynchronous active-low reset.
module MW_sqrt_pipe #(
    parameter int WIDTH  = 8,
    parameter int STAGES = 2,
    localparam int RW    = (WIDTH + 1) / 2
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             en,
    input  logic [WIDTH-1:0] a,
    output logic [RW-1:0]    root,
    output logic [RW:0]      remainder
);
    localparam int W = 2 * RW + 1;
    logic [RW-1:0] q;
    logic [RW:0]   r;
    MW_sqrt #(.WIDTH(WIDTH)) u_sqrt (.a, .root(q), .remainder(r));
    generate
        if (STAGES == 0) begin : g_comb
            assign {root, remainder} = {q, r};
        end else begin : g_reg
            logic [W-1:0] s [STAGES];
            always_ff @(posedge clk or negedge rst_n) begin
                if (!rst_n) begin
                    for (int i = 0; i < STAGES; i++) s[i] <= '0;
                end else if (en) begin
                    s[0] <= {q, r};
                    for (int i = 1; i < STAGES; i++) s[i] <= s[i-1];
                end
            end
            assign {root, remainder} = s[STAGES-1];
        end
    endgenerate
endmodule
