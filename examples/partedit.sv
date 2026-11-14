// msim -compile spreads the generated code over several C++ files and
// rebuilds only those whose text changed. An edit to one module (here
// +define+EDIT, one more term in `mixer`) must change only the files of
// mixer's code (its 2-state and 4-state copies): the instances of a process
// share a file, and no other chunk moves (tests/msim_parts.cmake).
module counter #(parameter int STEP = 1) (input logic clk, rst, output logic [15:0] q);
    always_ff @(posedge clk) if (rst) q <= 0; else q <= q + 16'(STEP);
endmodule
module mixer (input logic clk, rst, input logic [15:0] a, b, output logic [15:0] y);
    localparam logic [15:0] K = 16'h1234;
`ifdef EDIT
    always_ff @(posedge clk) if (rst) y <= K; else y <= {y[14:0], y[15]} ^ a ^ (b + K) ^ {a[7:0], b[15:8]};
`else
    always_ff @(posedge clk) if (rst) y <= K; else y <= {y[14:0], y[15]} ^ a ^ (b + K);
`endif
endmodule
module shifter (input logic clk, rst, input logic [15:0] d, output logic [15:0] s);
    always_ff @(posedge clk) if (rst) s <= 0; else s <= (s << 3) ^ d ^ 16'h0f0f;
endmodule
module partedit;
    logic clk = 0, rst = 1;
    logic [15:0] q [4], y [4], s [4];
    for (genvar i = 0; i < 4; i++) begin : g
        counter #(.STEP(i + 1)) c(.clk(clk), .rst(rst), .q(q[i]));
        mixer m(.clk(clk), .rst(rst), .a(q[i]), .b(s[i]), .y(y[i]));
        shifter h(.clk(clk), .rst(rst), .d(y[i]), .s(s[i]));
    end
    always #5 clk = ~clk;
    initial begin
        #12 rst = 0;
        #1000 $display("%h %h %h %h", y[0], y[1], s[2], s[3]);
        $finish;
    end
endmodule
