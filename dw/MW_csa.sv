// MW_csa - carry save adder (3:2 compressor row).
//   a + b + c = sum + carry, with carry already shifted left one place.
//   The carry out of the top bit is dropped, so the identity holds modulo
//   2^WIDTH; co is that dropped bit.
module MW_csa #(
    parameter int WIDTH = 8
) (
    input  logic [WIDTH-1:0] a,
    input  logic [WIDTH-1:0] b,
    input  logic [WIDTH-1:0] c,
    output logic [WIDTH-1:0] sum,
    output logic [WIDTH-1:0] carry,
    output logic             co
);
    logic [WIDTH-1:0] maj;
    assign sum   = a ^ b ^ c;
    assign maj   = (a & b) | (a & c) | (b & c);
    assign carry = {maj[WIDTH-2:0], 1'b0};
    assign co    = maj[WIDTH-1];
    generate
        if (WIDTH < 2) begin : g_bad
            $error("MW_csa: WIDTH must be at least 2");
        end
    endgenerate
endmodule
