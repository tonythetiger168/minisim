// IEEE 1800-2023 11.4.11, Table 11-20: with an x or z condition both arms are
// evaluated and combined bit by bit. Only bits that are 0 in both or 1 in
// both survive; every other pair, z with z included, gives x.
//! icarus-skip: Icarus 13 keeps z where both arms are z
module top;
    reg c;
    reg [3:0] a, b;
    wire [3:0] w = c ? a : b;
    initial begin
        c = 1'bx;
        a = 4'bzz10; b = 4'bz010;
        #1 $display("%b %b", c ? a : b, w);
        c = 1'bz;
        a = 4'bzzzz; b = 4'bzzzz;
        #1 $display("%b %b", c ? a : b, w);
        $display("%b", c ? 1'bz : 1'bz);
    end
endmodule
//> xx10 xx10
//> xxxx xxxx
//> x
