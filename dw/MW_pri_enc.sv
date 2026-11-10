// MW_pri_enc - priority encoder.
//   index = position of the lowest (LSB_FIRST = 1) or highest (LSB_FIRST = 0)
//   1 bit of a. valid = 0 and index = 0 when a is 0.
module MW_pri_enc #(
    parameter int WIDTH     = 8,
    parameter bit LSB_FIRST = 1,
    localparam int IW       = (WIDTH > 1) ? $clog2(WIDTH) : 1
) (
    input  logic [WIDTH-1:0] a,
    output logic [IW-1:0]    index,
    output logic             valid
);
    always_comb begin
        index = '0;
        valid = 1'b0;
        if (LSB_FIRST) begin
            for (int i = WIDTH - 1; i >= 0; i--)
                if (a[i]) begin index = IW'(i); valid = 1'b1; end
        end else begin
            for (int i = 0; i < WIDTH; i++)
                if (a[i]) begin index = IW'(i); valid = 1'b1; end
        end
    end
endmodule
