// 6.24.1: a cast assigns its operand to the cast type, so a narrow unsigned
// operand is zero-extended to int even when the cast sits in a signed
// expression that is no wider than int.
//> 6 1 4 4
//> -2 3
module t;
    logic [2:0] u;
    logic signed [2:0] s;
    int k;
    initial begin
        u = 6; s = -3'sd2; k = 0;
        $display("%0d %0d %0d %0d", int'(u), int'(u) % 5, k + 5 - int'(u) % 5, (k + 5 - int'(u) % 5) % 5);
        $display("%0d %0d", int'(s) % 5, (k + int'(s) + 5) % 5);
    end
endmodule
