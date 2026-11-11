module lram_tb;
    logic clk = 0, we = 0, twe = 0;
    logic [3:0] wa = 0, ra = 0, rb = 0;
    logic [7:0] wd = 0;
    logic [5:0] ta = 0, tr = 0;
    logic [4:0] td = 0;
    wire [7:0] qa, qb, sq;
    wire [4:0] tq;
    lram dut (.*);
    int unsigned seed = 32'h1badf00d;
    function automatic int unsigned rnd();
        seed = seed * 1664525 + 1013904223;
        return seed >> 7;
    endfunction
    initial begin
        // fill the memories: a word never written is x in RTL and 0 in a LUT RAM
        for (int i = 0; i < 64; i++) begin
            we = 1; wa = i; wd = rnd(); twe = 1; ta = i; td = rnd();
            #5 clk = 1; #5 clk = 0;
        end
        for (int i = 0; i < 600; i++) begin
            we = rnd() & 1; twe = rnd() & 1;
            wa = rnd(); ra = (rnd() & 1) ? wa : rnd(); rb = rnd(); wd = rnd();
            ta = rnd(); tr = (rnd() & 1) ? ta : rnd(); td = rnd();
            #1 $display("%0d a qa=%h qb=%h tq=%h sq=%h", i, qa, qb, tq, sq);
            #4 clk = 1;
            #1 $display("%0d b qa=%h qb=%h tq=%h sq=%h", i, qa, qb, tq, sq);
            #4 clk = 0;
            #1 $display("%0d c qa=%h qb=%h tq=%h", i, qa, qb, tq);
        end
        $finish;
    end
endmodule
