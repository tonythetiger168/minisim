// MW_mux - NUM-to-1 multiplexer.
//   in = {input[NUM-1], ..., input[0]}, WIDTH bits each. out = input[sel];
//   a sel of NUM or more gives 0.
module MW_mux #(
    parameter int WIDTH = 8,
    parameter int NUM   = 4,
    localparam int SW   = (NUM > 1) ? $clog2(NUM) : 1
) (
    input  logic [NUM*WIDTH-1:0] in,
    input  logic [SW-1:0]        sel,
    output logic [WIDTH-1:0]     out
);
    always_comb begin
        out = '0;
        for (int i = 0; i < NUM; i++)
            if (sel == SW'(i)) out = in[i*WIDTH +: WIDTH];
    end
endmodule
