// Multipliers for the DSP tests: unsigned, signed, mixed widths, a result
// narrower and one wider than the product, a multiply-add, and one too wide
// for a block (it stays LUTs).
module mul (
    input  wire               clk, clr,
    input  wire        [7:0]  a, b,
    input  wire        [8:0]  sa,     // multiplied as signed
    input  wire        [5:0]  sb,
    input  wire        [10:0] c,
    input  wire        [27:0] w,
    output reg         [15:0] pu,
    output reg         [19:0] ps,
    output reg         [11:0] plow,
    output reg         [20:0] pacc,
    output wire        [55:0] pw
);
    assign pw = w * w;
    always @(posedge clk) begin
        pu <= a * b;
        ps <= $signed(sa) * $signed(sb);
        plow <= c * a;
        pacc <= clr ? 21'd0 : pacc + a * c;
    end
endmodule
