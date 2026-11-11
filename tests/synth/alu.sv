// Arithmetic, shifts, comparisons, signed operators, case, functions.
module alu (
    input  logic [3:0]  op,
    input  logic [15:0] a, b,
    output logic [15:0] y,
    output logic        zero, lt, ltu
);
    function automatic logic [15:0] rotl(input logic [15:0] v, input logic [3:0] n);
        return (v << n) | (v >> (5'd16 - n));
    endfunction
    function automatic logic [4:0] popcount(input logic [15:0] v);
        popcount = '0;
        for (int i = 0; i < 16; i++) popcount += v[i];
    endfunction
    always_comb begin
        unique case (op)
            4'd0: y = a + b;
            4'd1: y = a - b;
            4'd2: y = a & b;
            4'd3: y = a | b;
            4'd4: y = a ^ b;
            4'd5: y = a << b[3:0];
            4'd6: y = a >> b[3:0];
            4'd7: y = $signed(a) >>> b[3:0];
            4'd8: y = a * b;
            4'd9: y = (b == 0) ? 16'hffff : a / b;
            4'd10: y = (b == 0) ? a : a % b;
            4'd11: y = rotl(a, b[3:0]);
            4'd12: y = {11'b0, popcount(a)};
            4'd13: y = ($signed(a) < $signed(b)) ? a : b;
            4'd14: y = {a[7:0], b[15:8]};
            default: y = ~a;
        endcase
    end
    assign zero = ~|y;
    assign lt = $signed(a) < $signed(b);
    assign ltu = a < b;
endmodule
