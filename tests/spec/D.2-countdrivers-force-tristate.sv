// IEEE 1800-2023 D.2: $countdrivers counts the drivers of a net. A force is
// not a driver: an undriven net stays at 0 drivers while forced, and a driven
// one keeps its count. A three-state gate is one driver, off while disabled,
// and an x it drives counts once as an x driver.
module top;
    wire n0, n1, t;
    reg d, en;
    integer m, f, c, c0, c1, cx;
    assign n1 = 1'b0;
    bufif1 (t, d, en);
    assign t = 1'b1;
    initial begin
        d = 1'bx; en = 0;
        #1 force n0 = 1'bx;
        force n1 = 1'b1;
        #1 m = $countdrivers(n0, f, c, c0, c1, cx);
        $display("%0d %0d %0d %0d %0d %0d", m, f, c, c0, c1, cx);
        m = $countdrivers(n1, f, c, c0, c1, cx);
        $display("%0d %0d %0d %0d %0d %0d", m, f, c, c0, c1, cx);
        m = $countdrivers(t, f, c, c0, c1, cx);
        $display("%0d %0d %0d %0d %0d %0d", m, f, c, c0, c1, cx);
        en = 1;
        #1 m = $countdrivers(t, f, c, c0, c1, cx);
        $display("%0d %0d %0d %0d %0d %0d", m, f, c, c0, c1, cx);
        d = 0;
        #1 m = $countdrivers(t, f, c, c0, c1, cx);
        $display("%0d %0d %0d %0d %0d %0d", m, f, c, c0, c1, cx);
    end
endmodule
//> 0 1 0 0 0 0
//> 0 1 1 1 0 0
//> 0 0 1 0 1 0
//> 1 0 2 0 1 1
//> 1 0 2 1 1 0
