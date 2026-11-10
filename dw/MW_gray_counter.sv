// MW_gray_counter - Gray code counter: one bit of count changes per step.
//   bin is the same count in binary.
module MW_gray_counter #(
    parameter int WIDTH = 4
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             en,
    output logic [WIDTH-1:0] count,
    output logic [WIDTH-1:0] bin
);
    logic [WIDTH-1:0] nxt;
    MW_inc #(.WIDTH(WIDTH)) u_inc (.a(bin), .sum(nxt));
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            bin   <= '0;
            count <= '0;
        end else if (en) begin
            bin   <= nxt;
            count <= nxt ^ (nxt >> 1);
        end
    end
endmodule
