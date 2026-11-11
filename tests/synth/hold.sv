// Registers whose "keep the value" case sits under the selects of an
// if/case tree, not at the top: msynth gathers the hold condition into one
// enable and drops the held leaves from the data logic.
module hold (
    input  logic       clk, rst, a, b, c,
    input  logic [1:0] k,
    input  logic [7:0] x, y,
    output logic [7:0] r, s, t
);
    always_ff @(posedge clk) begin
        // loads under a or b: one enable, one select a bit
        if (a) r <= x;
        else if (b) r <= y;
        // synchronous reset in front, an explicit hold in the middle
        if (rst) s <= '0;
        else if (a) s <= x;
        else if (b) s <= s;
        else if (c) s <= y;
        // a case with holding items
        case (k)
            2'd0: t <= x;
            2'd1: if (c) t <= y;
            2'd2: t <= t;
            default: if (a & b) t <= x ^ y;
        endcase
    end
endmodule
