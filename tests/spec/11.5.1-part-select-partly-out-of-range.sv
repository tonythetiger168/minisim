// IEEE 1800-2023 11.5.1, 7.4.6: a constant part select that is partly outside
// the declared range is legal (a warning). A write changes only the bits in
// range and a read gives x for the bits outside it.
module top;
    reg [3:0] x;
    integer i;
    initial begin
        x = 'x; x[4:-1] = 6'b101010; $display("%b", x);
        x = 'x; x[0:-1] = 2'b10;     $display("%b", x);
        x = 'x; x[5:3] = 3'b001;     $display("%b", x);
        x = 4'b0110; $display("%b %b", x[5:2], x[1:-2]);
        i = -1; x = 'x; x[i+:3] = 3'b101; $display("%b", x);
    end
endmodule
//> 0101
//> xxx1
//> 1xxx
//> xx01 10xx
//> xx10
