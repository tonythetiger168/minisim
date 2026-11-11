module mul_tb;
    logic clk = 0, clr = 1;
    logic [7:0] a = 0, b = 0;
    logic [8:0] sa = 0;
    logic [5:0] sb = 0;
    logic [10:0] c = 0;
    logic [27:0] w = 0;
    wire [15:0] pu;
    wire [19:0] ps;
    wire [11:0] plow;
    wire [20:0] pacc;
    wire [55:0] pw;
    mul dut (.*);
    int unsigned seed = 32'h0badcafe;
    function automatic int unsigned rnd();
        seed = seed * 1664525 + 1013904223;
        return seed >> 6;
    endfunction
    initial begin
        for (int i = 0; i < 300; i++) begin
            a = rnd(); b = rnd(); sa = rnd(); sb = rnd(); c = rnd(); w = rnd();
            if (i % 50 == 7) begin a = 8'hff; b = 8'hff; sa = 9'h100; sb = 6'h20; c = 11'h7ff; w = 28'hfffffff; end
            if (i % 50 == 8) begin sa = 9'h0ff; sb = 6'h20; end
            clr = i == 0 || (rnd() % 41) == 0;
            #5 clk = 1; #5 clk = 0;
            if (i > 0) $display("%0d pu=%h ps=%h plow=%h pacc=%h pw=%h", i, pu, ps, plow, pacc, pw);
        end
        $finish;
    end
endmodule
