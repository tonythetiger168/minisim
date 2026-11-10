module counter_tb;
    logic clk = 0, rst_n = 0, en = 0, load = 0;
    logic [7:0] d = 0;
    wire [7:0] q;
    wire wrap;
    counter dut (.*);
    always #5 clk = ~clk;
    int seed = 1;
    initial begin
        #12 rst_n = 1;
        repeat (600) begin
            @(negedge clk);
            $display("%h %b", q, wrap);
            en = ($urandom(seed) % 8) != 0;
            load = ($urandom(seed) % 64) == 0;
            d = $urandom(seed);
            if (($urandom(seed) % 200) == 0) begin rst_n = 0; #1 rst_n = 1; end
        end
        $finish;
    end
endmodule
