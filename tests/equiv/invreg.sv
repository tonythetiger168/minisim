// A counter that resets to all ones. invreg_gates.v builds it from flops that
// can only reset to 0, so it stores the count inverted.
module invreg (input logic clk, rst_n, en, output logic [2:0] q);
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) q <= '1;
        else if (en) q <= q + 3'd1;
endmodule
