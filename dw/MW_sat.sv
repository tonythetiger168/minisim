// MW_sat - narrows a to OUT_WIDTH bits, clipping instead of wrapping.
//   tc = 0: unsigned, clips to 2^OUT_WIDTH-1.
//   tc = 1: two's complement, clips to the most positive or most negative
//   OUT_WIDTH-bit value. sat = 1 when it clipped.
module MW_sat #(
    parameter int IN_WIDTH  = 16,
    parameter int OUT_WIDTH = 8
) (
    input  logic [IN_WIDTH-1:0]  a,
    input  logic                 tc,
    output logic [OUT_WIDTH-1:0] out,
    output logic                 sat
);
    logic [IN_WIDTH-OUT_WIDTH:0] top;      // the dropped bits and the new sign bit
    assign top = a[IN_WIDTH-1:OUT_WIDTH-1];
    always_comb begin
        out = a[OUT_WIDTH-1:0];
        sat = 1'b0;
        if (tc) begin
            if (!(&top) && (|top)) begin
                sat = 1'b1;
                out = {a[IN_WIDTH-1], {(OUT_WIDTH-1){~a[IN_WIDTH-1]}}};
            end
        end else if (|a[IN_WIDTH-1:OUT_WIDTH]) begin
            sat = 1'b1;
            out = '1;
        end
    end
    generate
        if (IN_WIDTH <= OUT_WIDTH || OUT_WIDTH < 2) begin : g_bad
            $error("MW_sat: need IN_WIDTH > OUT_WIDTH >= 2");
        end
    endgenerate
endmodule
