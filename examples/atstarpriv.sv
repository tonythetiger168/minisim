// `always @*` blocks whose inputs are registers private to msim -compile's
// cycle domain (kept outside S.sigs). An @* block does not run at time 0
// (9.4.2.2): it first runs when one of its inputs changes. `y` follows a
// counter and must update; `z` reads a register that is written every
// cycle with its own value, so it never changes and `z` stays x. The
// interpreter and -compile must print the same.
module atstarpriv;
    reg clk = 0;
    reg rst = 1;
    reg [7:0] cnt;
    reg [7:0] k = 8'd5;
    reg [7:0] y, z, w;
    reg [15:0] acc;
    always #5 clk = ~clk;
    always @(posedge clk) if (rst) cnt <= 0; else cnt <= cnt + 8'd1;
    always @(posedge clk) k <= k;
    always @(posedge clk) if (rst) acc <= 0; else acc <= {acc[14:0], acc[15]} ^ {8'd0, y} ^ {w, 8'd0};
    always @* y = cnt * 8'd3;
    always @* z = k + 8'd1;
    always @* begin w = y ^ 8'h5a; if (cnt[0]) w = ~w; end
    initial begin
        repeat (3) @(posedge clk);
        rst = 0;
        repeat (40) @(posedge clk);
        #1 $display("y=%h z=%h w=%h acc=%h", y, z, w, acc);
        $finish;
    end
endmodule
