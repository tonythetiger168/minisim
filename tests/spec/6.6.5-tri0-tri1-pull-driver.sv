// IEEE 1800-2023 6.6.5: a tri0 (tri1) net is a wire with a pull 0 (pull 1)
// always driving it. A pull driver of the other value therefore makes it
// x, a weaker one loses, a strong one wins, and with no driver it is the pull.
module top;
    tri1 a, c, e;
    tri0 b, d, f;
    assign (pull1, pull0) a = 1'b0;
    assign (pull1, pull0) b = 1'b1;
    assign (weak1, weak0) c = 1'b0;
    assign (weak1, weak0) d = 1'b1;
    assign f = 1'b1;
    initial begin
        #1 $display("%b %b %b %b %b %b", a, b, c, d, e, f);
        $display("%v %v %v %v", a, c, e, f);
    end
endmodule
//> x x 1 0 1 1
//> PuX Pu1 Pu1 St1
