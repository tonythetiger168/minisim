module mulp_tb;
    logic clk = 0;
    logic [4:0] a = 0, b = 0;
    logic [5:0] c = 0;
    logic [3:0] d = 0;
    wire [9:0] ps, pu;
    wire [6:0] pm;
    mulp dut (.*);
    initial begin
        for (int i = 0; i < 1024; i++) begin
            {a, b} = i; c = i * 37 + (i >> 4); d = i * 11 + (i >> 6);
            #5 clk = 1; #5 clk = 0;
            $display("%0d ps=%h pu=%h pm=%h", i, ps, pu, pm);
        end
        $finish;
    end
endmodule
