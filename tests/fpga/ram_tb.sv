module ram_tb;
    logic clk = 0, we = 0, re = 0, rst = 1, push = 0, pop = 0, swe = 0;
    logic [5:0] wa = 0, ra = 0, rb = 0;
    logic [7:0] din = 0;
    logic [11:0] wd = 0;
    logic [3:0] sa = 0, sd = 0;
    wire [11:0] qa, qb;
    wire [7:0] dout;
    wire empty;
    wire [3:0] sq;
    ram dut (.*);
    int unsigned seed = 32'hc0ffee11;
    function automatic int unsigned rnd();
        seed = seed * 1664525 + 1013904223;
        return seed >> 7;
    endfunction
    // an output counts once it has been loaded: before that it is x in RTL and anything in a RAM
    bit qa_ok = 0, qb_ok = 0, dout_ok = 0;
    int level = 0;
    initial begin
        // fill both memories first
        for (int i = 0; i < 64; i++) begin
            we = 1; wa = i; wd = rnd(); swe = i < 16; sa = i; sd = rnd();
            rst = i < 2; push = 0; pop = 0; re = 0; rb = 0;
            #5 clk = 1; #5 clk = 0;
        end
        for (int i = 0; i < 1500; i++) begin
            we = (rnd() % 3) != 0; re = (rnd() % 4) != 0; swe = rnd() & 1;
            // a small address range: reads and writes of one word in one cycle are common
            wa = rnd() % 12; ra = rnd() % 12; rb = (rnd() & 1) ? wa : rnd();
            wd = rnd(); sa = rnd(); sd = rnd(); din = rnd();
            push = (rnd() % 3) != 0 && level < 128;
            pop = (rnd() % 3) != 0 && level > 0;
            if (i % 600 > 300) pop = level > 0;        // run it empty: read follows write at once
            #5 clk = 1; #5 clk = 0;
            if (re) qa_ok = 1;
            qb_ok = 1;
            if (pop) dout_ok = 1;
            level = level + push - pop;
            $display("%0d qa=%h qb=%h dout=%h empty=%b sq=%h", i, qa_ok ? qa : 12'd0, qb_ok ? qb : 12'd0,
                     dout_ok ? dout : 8'd0, empty, sq);
        end
        $finish;
    end
endmodule
