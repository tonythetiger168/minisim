// IEEE 1800-2023 9.4.5, 10.4.2: a nonblocking assignment with an
// intra-assignment event control may write a concatenation of vectors,
// part selects and array elements; each part takes its slice of the value
// taken when the statement runs. Index expressions on the left-hand side
// are evaluated with the right-hand side, not when the event comes.
module top;
    reg [3:0] x, y;
    reg [3:0] z [1:0];
    event e;
    integer i;
    initial begin
        {z[1][1:0], z[0], y[1:0], x} <= @e 12'h5a5;
        #1 $display("%b %b %b %b", z[1], z[0], y, x);
        ->e;
        #1 $display("%b %b %b %b", z[1], z[0], y, x);
        i = 1;
        {x, z[i]} <= repeat (2) @e 8'h3c;
        i = 0;
        x[i] <= @e 1'b1;
        i = 3;
        #1 ->e;
        #1 $display("%b %b", x, z[1]);
        ->e;
        #1 $display("%b %b %b", x, z[1], z[0]);
    end
endmodule
//> xxxx xxxx xxxx xxxx
//> xx01 0110 xx10 0101
//> 0101 xx01
//> 0011 1100 0110
