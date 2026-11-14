// $fatal(2) ends the run with exit status 1 in every mode (20.10); tests/msim_compile.cmake.
module fat;
    logic clk = 0;
    int n = 0;
    always #5 clk = ~clk;
    always @(posedge clk) begin
        n <= n + 1;
        if (n == 7) $fatal(2, "n reached %0d", n);
    end
endmodule
