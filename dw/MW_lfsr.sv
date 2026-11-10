// MW_lfsr - Galois linear feedback shift register.
//   Each en step shifts right; when the bit shifted out is 1, POLY is
//   XORed in. POLY lists the taps with bit WIDTH-1 for the x^WIDTH term
//   (so it is always 1). The default, 8'hB8, is x^8+x^6+x^5+x^4+1 and
//   runs through all 255 non-zero states. load takes data; a state of 0
//   stays 0. Reset gives 1.
module MW_lfsr #(
    parameter int               WIDTH = 8,
    parameter logic [WIDTH-1:0] POLY  = 8'hB8
) (
    input  logic             clk,
    input  logic             rst_n,
    input  logic             load,
    input  logic [WIDTH-1:0] data,
    input  logic             en,
    output logic [WIDTH-1:0] q
);
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n)    q <= WIDTH'(1);
        else if (load) q <= data;
        else if (en)   q <= (q >> 1) ^ (q[0] ? POLY : '0);
    end
endmodule
