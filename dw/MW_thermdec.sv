// MW_thermdec - binary to thermometer code decoder.
//   thermo[i] = en && (i < a): a ones from the bottom, the rest zeros.
//   a = 0 or en = 0 gives all zeros; a >= 2^WIDTH cannot happen.
module MW_thermdec #(
    parameter int WIDTH = 3,
    localparam int N    = 1 << WIDTH
) (
    input  logic [WIDTH-1:0] a,
    input  logic             en,
    output logic [N-1:0]     thermo
);
    always_comb
        for (int i = 0; i < N; i++) thermo[i] = en && (i < a);
endmodule
