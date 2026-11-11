module ramw_tb;
    logic clk = 0, we = 0, dwe = 0;
    logic [5:0] wa = 0, ra = 0;
    logic [17:0] wd = 0;
    logic [10:0] dwa = 0, dra = 0;
    logic [8:0] dwd = 0;
    wire [17:0] q, nq;
    wire [8:0] dq;
    ramw dut (.*);
    int unsigned seed = 32'h5eed1234;
    function automatic int unsigned rnd();
        seed = seed * 1664525 + 1013904223;
        return seed >> 7;
    endfunction
    initial begin
        // fill the memories: a word never written is x in RTL and anything in a RAM
        for (int i = 0; i < 2048; i++) begin
            we = i < 64; wa = i; wd = rnd(); dwe = 1; dwa = i; dwd = rnd();
            #5 clk = 1; #5 clk = 0;
        end
        for (int i = 0; i < 800; i++) begin
            we = (rnd() % 3) != 0; dwe = (rnd() % 3) != 0;
            wa = rnd() % 10; wd = rnd(); dwd = rnd();
            // the falling-edge read never looks at the word written on the edge before it
            ra = (wa + 1 + rnd() % 8) % 10;
            dwa = (rnd() & 1) ? rnd() % 16 : rnd();
            dra = (rnd() & 1) ? dwa : (rnd() & 1) ? rnd() % 16 : rnd();
            #5 clk = 1; #5 clk = 0;
            $display("%0d q=%h dq=%h nq=%h", i, i ? q : 18'd0, i ? dq : 9'd0, nq);
        end
        $finish;
    end
endmodule
