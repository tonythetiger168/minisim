module dp_tb;
    logic clk = 0, rst = 1, en = 0, sub = 0, load = 0;
    logic [11:0] a = 0, b = 0;
    wire [11:0] acc, cnt;
    wire ltu, lts, zero;
    wire [12:0] sum;
    dp dut (.*);
    int unsigned seed = 32'h1234abcd;
    function automatic int unsigned rnd();
        seed = seed * 1664525 + 1013904223;
        return seed >> 8;
    endfunction
    initial begin
        repeat (3) begin #5 clk = 1; #5 clk = 0; end
        for (int i = 0; i < 400; i++) begin
            rst = (rnd() % 37) == 0;
            en = (rnd() % 4) != 0;
            sub = rnd() & 1;
            load = (rnd() % 9) == 0;
            a = (rnd() % 5) == 0 ? b : rnd();
            b = rnd();
            if (i > 300) b = 12'd4095 - (i & 3);
            #5 clk = 1; #5 clk = 0;
            $display("%0d acc=%h cnt=%h ltu=%b lts=%b zero=%b sum=%h", i, acc, cnt, ltu, lts, zero, sum);
        end
        $finish;
    end
endmodule
