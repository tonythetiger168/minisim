// IEEE 1800-2023 28.8: the terminals of a bidirectional switch may be bit
// selects of nets, also two bits of one net. Only those bits are joined;
// the others resolve on their own.
module top;
    reg d, en;
    tri [3:0] a;
    tri b;
    wire [5:0] c;
    assign a[0] = d;
    tran (a[0], a[2]);
    tranif1 (a[2], a[3], en);
    tran (a[3], b);
    assign c[1:0] = 2'b10;
    rtran (c[1], c[5]);
    initial begin
        d = 1; en = 0;
        #1 $display("%b %b %b %v", a, b, c, c[5]);
        en = 1;
        #1 $display("%b %b", a, b);
        d = 0;
        #1 $display("%b %b", a, b);
    end
endmodule
//> z1z1 z 1zzz10 Pu1
//> 11z1 1
//> 00z0 0
