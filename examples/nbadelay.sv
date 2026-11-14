// x <= #d v and #(expr) delays known only at run time (9.4.5, 9.4.1), in -compile too.
module nd;
    logic clk = 0;
    logic [7:0] a = 0, b = 0, c = 0;
    int half = 5;
    realtime per = 10.0;
    always begin #(half) clk = ~clk; end
    always @(posedge clk) begin
        a <= #3 a + 1;
        b <= #(half - 2) b + a;
        c = #1 c ^ b;
    end
    initial begin
        clk <= #(per / 2) 1'b0;
        #200 $display("%0t a=%0d b=%0d c=%0d", $time, a, b, c);
        half = 7;
        #100 $display("%0t a=%0d b=%0d c=%0d", $time, a, b, c);
        $finish;
    end
endmodule
