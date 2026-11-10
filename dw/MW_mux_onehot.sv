// MW_mux_onehot - AND-OR multiplexer with a one-hot select.
//   out = OR of every input whose sel bit is 1 (0 when none is).
module MW_mux_onehot #(
    parameter int WIDTH = 8,
    parameter int NUM   = 4
) (
    input  logic [NUM*WIDTH-1:0] in,
    input  logic [NUM-1:0]       sel,
    output logic [WIDTH-1:0]     out
);
    always_comb begin
        out = '0;
        for (int i = 0; i < NUM; i++)
            out = out | (in[i*WIDTH +: WIDTH] & {WIDTH{sel[i]}});
    end
endmodule
