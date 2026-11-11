module mulw_tb;
    logic clk = 0;
    logic [31:0] a = 0, b = 0;
    logic [39:0] c = 0;
    logic [32:0] d = 0;
    wire [63:0] pu;
    wire [43:0] ps;
    wire [47:0] pt, sq;
    mulw dut (.*);
    int unsigned seed = 32'h5eed1234;
    function automatic int unsigned rnd();
        seed = seed * 1664525 + 1013904223;
        return seed ^ (seed >> 15);
    endfunction
    initial begin
        for (int i = 0; i < 400; i++) begin
            a = rnd(); b = rnd(); c = {rnd(), rnd()}; d = {rnd(), rnd()};
            case (i % 40)
                3: begin a = '1; b = '1; c = '1; d = '1; end                       // all ones: -1 or the largest
                4: begin a = 32'h80000000; b = 32'h80000000; c = 40'h8000000000; d = 33'h100000000; end   // most negative
                5: begin a = 32'h7fffffff; b = 32'h80000000; c = 40'h7fffffffff; d = 33'h100000000; end
                6: begin a = 32'h00800000; b = 32'h00080000; c = 40'h8000000000; d = 33'h0ffffffff; end   // sign bits of the slices
                7: begin a = 32'h0003ffff; b = 32'h00020000; c = 40'h000001ffff; d = 33'h00001ffff; end   // block boundaries
                8: begin a = 0; b = '1; c = 0; d = '1; end
                default: ;
            endcase
            #5 clk = 1; #5 clk = 0;
            $display("%0d pu=%h ps=%h pt=%h sq=%h", i, pu, ps, pt, sq);
        end
        $finish;
    end
endmodule
