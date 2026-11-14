// A clocked block that calls a task that does nothing, as PicoRV32's
// `assert does outside formal builds. The call is dropped (msim.cpp
// dropEmptyCalls), so the block can join the static schedule and the cycle
// domain; a task with a body is still called. Every mode must print the
// same (tests/msim_compile.cmake).
module emptycall;
    logic clk = 0, rst = 1;
    logic [7:0] a, b;
    int calls = 0;
    task empty_statement; begin end endtask
    task count; calls++; endtask
    always #5 clk = ~clk;
    always @(posedge clk) begin
        if (rst) a <= 0;
        else begin
            empty_statement;
            a <= a + 8'd3;
            if (a[0]) empty_statement;
        end
    end
    always @(posedge clk) begin
        if (rst) b <= 8'h11;
        else begin
            b <= {b[6:0], b[7] ^ a[2]};
            count;
        end
    end
    initial begin
        #22 rst = 0;
        #400 $display("a=%h b=%h calls=%0d", a, b, calls);
        $finish;
    end
endmodule
