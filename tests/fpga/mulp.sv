// Small multipliers forced onto DSP blocks (-dsp-min 4), small enough for a
// SAT proof against the vendors' simulation models.
module mulp (
    input  wire       clk,
    input  wire [4:0] a, b,
    input  wire [5:0] c,
    input  wire [3:0] d,
    output reg  [9:0] ps,
    output reg  [9:0] pu,
    output reg  [6:0] pm
);
    always @(posedge clk) begin
        ps <= $signed(a) * $signed(b);
        pu <= c * d;
        pm <= $signed(a) * $signed({1'b0, d});
    end
endmodule
