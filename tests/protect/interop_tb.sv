module interop_tb;
    reg clk = 0;
    reg [7:0] a = 8'h0f;
    wire [7:0] y;
    interop_ip u_ip (.clk(clk), .a(a), .y(y));
    initial begin
        #1 clk = 1;
        #1 $display("tb: y=%02h", y);
        $finish;
    end
endmodule
