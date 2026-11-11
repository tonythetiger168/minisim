// A counter with enable, synchronous load and asynchronous reset.
module counter #(parameter W = 8) (
    input  logic         clk, rst_n, en, load,
    input  logic [W-1:0] d,
    output logic [W-1:0] q,
    output logic         wrap
);
    always_ff @(posedge clk or negedge rst_n)
        if (!rst_n) q <= '0;
        else if (load) q <= d;
        else if (en) q <= q + 1'b1;
    assign wrap = en && (q == '1);
endmodule
