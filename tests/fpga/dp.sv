// A small datapath for the carry-chain and flop-pin tests: synchronous
// reset, clock enable, an accumulator that adds or subtracts, signed and
// unsigned compares, and a saturating down counter.
module dp #(parameter W = 12) (
    input  wire         clk, rst, en, sub, load,
    input  wire [W-1:0] a, b,
    output reg  [W-1:0] acc,
    output reg  [W-1:0] cnt,
    output reg          ltu, lts, zero,
    output wire [W:0]   sum
);
    assign sum = {1'b0, a} + {1'b0, b};
    always @(posedge clk)
        if (rst) begin
            acc <= {W{1'b0}};
            cnt <= {W{1'b1}};
            ltu <= 1'b0;
            lts <= 1'b0;
            zero <= 1'b1;
        end else if (en) begin
            if (load) acc <= a;
            else if (sub) acc <= acc - b;
            else acc <= acc + b;
            if (cnt != {W{1'b0}}) cnt <= cnt - 1'b1;
            ltu <= a < b;
            lts <= $signed(a) < $signed(b);
            zero <= acc == {W{1'b0}};
        end
endmodule
