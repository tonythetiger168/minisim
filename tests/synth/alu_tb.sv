module alu_tb;
    logic [3:0] op;
    logic [15:0] a, b;
    wire [15:0] y;
    wire zero, lt, ltu;
    alu dut (.*);
    int seed = 7;
    initial begin
        for (int i = 0; i < 3000; i++) begin
            op = i % 16;
            a = $urandom(seed);
            b = $urandom(seed);
            if (i % 5 == 0) b = b & 16'h000f;
            if (i % 7 == 0) a = b;
            #1 $display("%d %h %h %h %b%b%b", op, a, b, y, zero, lt, ltu);
        end
        $finish;
    end
endmodule
