// IEEE 1800-2023 10.6.2, 28.8: releasing a forced net hands it back to its
// drivers at once. A net joined to another by a tran switch takes the value
// of the whole group again when released.
module top;
    reg a;
    wire x, y, ab;
    assign (weak1, weak0) x = 1'b1;
    assign y = a;
    assign ab = ~a;
    tran (x, y);
    initial begin
        a = 0;
        #1 $display("%b %b", x, y);
        force x = ab;
        #1 $display("%b", x);
        release x;
        #1 $display("%b %b", x, y);
        a = 1;
        #1 $display("%b %b", x, y);
    end
endmodule
//> 0 0
//> 1
//> 0 0
//> 1 1
