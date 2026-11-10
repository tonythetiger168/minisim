// MW_norm - normalizer: shifts a left until its top bit is 1.
//   out = a << count, count as MW_lzd. all_0 = 1 (and out = 0) when a is 0.
module MW_norm #(
    parameter int WIDTH = 8,
    localparam int CW   = $clog2(WIDTH + 1)
) (
    input  logic [WIDTH-1:0] a,
    output logic [WIDTH-1:0] out,
    output logic [CW-1:0]    count,
    output logic             all_0
);
    MW_lzd #(.WIDTH(WIDTH)) u_lzd (.a(a), .count(count), .all_0(all_0));
    MW_shifter #(.WIDTH(WIDTH), .SH_WIDTH(CW)) u_sh (
        .data(a), .sh(count), .dir(1'b0), .arith(1'b0), .out(out));
endmodule
