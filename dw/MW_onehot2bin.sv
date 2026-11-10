// MW_onehot2bin - one-hot to binary encoder.
//   index = position of the 1 bit. With more than one bit set the result is
//   the OR of their positions; error flags that case and a = 0.
module MW_onehot2bin #(
    parameter int WIDTH = 8,
    localparam int IW   = (WIDTH > 1) ? $clog2(WIDTH) : 1
) (
    input  logic [WIDTH-1:0] a,
    output logic [IW-1:0]    index,
    output logic             error
);
    always_comb begin
        logic seen, multi;
        index = '0;
        seen  = 1'b0;
        multi = 1'b0;
        for (int i = 0; i < WIDTH; i++) begin
            if (a[i]) begin
                index = index | IW'(i);
                multi = multi | seen;
                seen  = 1'b1;
            end
        end
        error = multi | ~seen;
    end
endmodule
