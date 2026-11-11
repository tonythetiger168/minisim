module sweep_tb;
    logic clk = 0, rst_n = 0, en = 1, sel = 1;
    logic [3:0] idx = 0;
    logic [7:0] d = 0;
    wire [7:0] q, pick;
    wire flag;
    sweep dut (.*);
    always #5 clk = ~clk;
    int seed = 3;
    initial begin
        // every register is written once before anything is printed
        #12 rst_n = 1;
        repeat (16) begin
            @(negedge clk);
            idx = idx + 1;
            d = $urandom(seed);
        end
        repeat (800) begin
            @(negedge clk);
            $display("%h %h %b", q, pick, flag);
            en = ($urandom(seed) % 4) != 0;
            sel = $urandom(seed);
            idx = $urandom(seed);
            d = $urandom(seed);
            if (($urandom(seed) % 100) == 0) begin rst_n = 0; #1 rst_n = 1; end
        end
        $finish;
    end
endmodule
