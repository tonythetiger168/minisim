// A small ALU with a registered result: the reference for alu_gates.v.
module alu #(parameter W = 4) (
    input  logic         clk, rst_n,
    input  logic [W-1:0] a, b,
    input  logic [1:0]   op,
    output logic [W-1:0] y,
    output logic         zero
);
    logic [W-1:0] r;
    always_comb begin
        case (op)
            2'd0: r = a + b;
            2'd1: r = a & b;
            2'd2: r = a ^ b;
            2'd3: r = ~a;
        endcase
    end
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) y <= '0;
        else y <= r;
    assign zero = (y == '0);
endmodule
