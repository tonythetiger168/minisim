module hier_tb;
    logic clk = 0;
    logic [11:0] a = 0, b = 0;
    logic [2:0] sel = 0;
    wire [11:0] sum, delayed;
    wire co;
    wire [3:0] first_one, nib;
    wire [4:0] evens;
    hier dut (.*);
    always #5 clk = ~clk;
    int seed = 11;
    int n = 0;
    initial begin
        repeat (1000) begin
            @(posedge clk);
            #1 a = $urandom(seed);
            b = $urandom(seed);
            sel = $urandom(seed) % 8;
            if (n % 9 == 0) a = 0;
            n++;
            #1 if (n > 4) $display("%h %b %d %h %d %h", sum, co, first_one, nib, evens, delayed);
        end
        $finish;
    end
endmodule
