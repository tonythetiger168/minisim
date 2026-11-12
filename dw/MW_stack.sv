// MW_stack - synchronous LIFO stack of DEPTH words.
//   push writes data_in on top, pop removes the top; data_out is the top
//   word (valid when empty = 0). Both in one cycle replace the top.
//   A push when full or a pop when empty is ignored and sets error for one
//   cycle. count is the number of words held.
//   rst_n is an asynchronous active-low reset.
module MW_stack #(
    parameter int WIDTH = 8,
    parameter int DEPTH = 8,
    localparam int CW   = $clog2(DEPTH + 1)
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             push,
    input  logic             pop,
    input  logic [WIDTH-1:0] data_in,
    output logic [WIDTH-1:0] data_out,
    output logic             empty,
    output logic             full,
    output logic [CW-1:0]    count,
    output logic             error
);
    logic [WIDTH-1:0] mem [DEPTH];
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            count <= '0;
            error <= 1'b0;
        end else begin
            error <= 1'b0;
            if (push && pop) begin
                if (count == 0) error <= 1'b1;
                else mem[count - 1] <= data_in;
            end else if (push) begin
                if (count == CW'(DEPTH)) error <= 1'b1;
                else begin
                    mem[count] <= data_in;
                    count <= count + 1'b1;
                end
            end else if (pop) begin
                if (count == 0) error <= 1'b1;
                else count <= count - 1'b1;
            end
        end
    end
    assign empty    = (count == 0);
    assign full     = (count == CW'(DEPTH));
    assign data_out = empty ? '0 : mem[count - 1];
    generate
        if (DEPTH < 1) begin : g_bad
            $error("MW_stack: DEPTH must be at least 1");
        end
    endgenerate
endmodule
