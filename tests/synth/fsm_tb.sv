module fsm_tb;
    logic clk = 0, rst = 1, start = 0;
    logic [7:0] din = 0;
    logic [1:0] addr = 0;
    wire [7:0] dout;
    wire busy, done, nflag, sflag;
    wire [2:0] prio;
    fsm dut (.*);
    always #5 clk = ~clk;
    int seed = 3;
    int n = 0;
    initial begin
        #12 rst = 0;
        repeat (1500) begin
            @(posedge clk);
            #2 start = ($urandom(seed) % 4) == 0;
            din = $urandom(seed);
            addr = $urandom(seed);
            n++;
            // the memory is not reset: show dout only once it has been written
            #1 $display("%b%b %d %b%b %h", busy, done, prio, nflag, sflag, n > 100 ? dout : 8'h0);
        end
        $finish;
    end
endmodule
