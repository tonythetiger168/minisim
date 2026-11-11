// Registers a synthesis tool takes out or folds: ones nothing reads, ones
// stuck at their reset value, load enables (nested conditions), and a
// 16:1 select that maps onto 4:1 mux cells.
module sweep (
    input  logic        clk, rst_n, en, sel,
    input  logic [3:0]  idx,
    input  logic [7:0]  d,
    output logic [7:0]  q,
    output logic [7:0]  pick,
    output logic        flag
);
    logic [7:0]  unread, unread2;   // a chain that reaches no output
    logic [3:0]  stuck;             // reset to 0, never loaded with anything else
    logic        hold1;             // reset to 1, only ever keeps its value
    logic [7:0]  r;                 // loads under two nested conditions, no reset
    logic [7:0]  bank [16];

    always_ff @(posedge clk) begin
        unread  <= d ^ unread2;
        unread2 <= unread + 8'd1;
    end
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) begin
            stuck <= '0;
            hold1 <= 1'b1;
        end else begin
            stuck <= stuck & {4{en}};
            hold1 <= hold1;
        end
    always_ff @(posedge clk)
        if (en) begin
            if (sel) r <= d;
            bank[idx] <= d ^ r;
        end
    assign q    = r | {4'b0, stuck};
    assign pick = bank[idx];
    assign flag = hold1 & (r == d);
endmodule
