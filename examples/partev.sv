// msim -compile partition test, event-driven (tests/msim_parts.cmake):
// unique modules with continuous assigns, each its own comb function.
// +define+SIG adds a signal and two processes to the first module; the
// parts of the other modules must stay the same (their comb functions
// were numbered by process, so every later one moved).
module front (input logic clk, rst, input logic [15:0] a, output logic [15:0] y);
`ifdef SIG
    logic [15:0] t;
    assign t = a ^ 16'h5a5a;
    always_ff @(posedge clk) if (rst) y <= 0; else y <= y + a + t;
`else
    always_ff @(posedge clk) if (rst) y <= 0; else y <= y + a;
`endif
endmodule
module mid1 (input logic [15:0] a, output logic [15:0] y); assign y = (a << 1) ^ 16'h1111; endmodule
module mid2 (input logic [15:0] a, output logic [15:0] y); assign y = (a >> 2) + 16'h2222; endmodule
module mid3 (input logic [15:0] a, output logic [15:0] y); assign y = a ^ {a[7:0], a[15:8]}; endmodule
module mid4 (input logic [15:0] a, output logic [15:0] y); assign y = a + 16'h0f0f - (a & 16'h00ff); endmodule
module mid5 (input logic [15:0] a, output logic [15:0] y); assign y = ~a ^ (a << 4); endmodule
module mid6 (input logic [15:0] a, output logic [15:0] y); assign y = a * 16'd3; endmodule
module partev;
    logic clk = 0, rst = 1;
    logic [15:0] q, r1, r2, r3, r4, r5, r6, acc;
    front f(.clk(clk), .rst(rst), .a(r6 ^ 16'h0001), .y(q));
    mid1 m1(.a(q), .y(r1));
    mid2 m2(.a(r1), .y(r2));
    mid3 m3(.a(r2), .y(r3));
    mid4 m4(.a(r3), .y(r4));
    mid5 m5(.a(r4), .y(r5));
    mid6 m6(.a(r5), .y(r6));
    always_ff @(posedge clk) if (rst) acc <= 0; else acc <= acc ^ r6;
    always #5 clk = ~clk;
    initial begin
        #12 rst = 0;
        #1000 $display("%h %h", q, acc);
        $finish;
    end
endmodule
