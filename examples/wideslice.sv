// Slices of up to 64 bits of a signal wider than 64 bits that stays in the
// kernel (here: a module output read by the testbench), within one limb
// and across two. msim -compile reads them from the limbs on its 2-state
// path; an X in one limb must still give X in the slices that cover it.
// The interpreter and -compile must print the same.
module wsrc(input logic clk, input logic [31:0] seed, output logic [111:0] cap);
    always_ff @(posedge clk) cap <= {cap[79:0], seed} ^ {seed, cap[111:32]};
endmodule
module wideslice;
    logic clk = 0;
    logic [31:0] seed;
    logic [111:0] cap;
    logic [34:0] a;
    logic [11:0] b;
    logic [63:0] c;
    logic d;
    logic [31:0] acc;
    wsrc u(.clk(clk), .seed(seed), .cap(cap));
    always #5 clk = ~clk;
    assign a = cap[34:0];
    assign b = cap[46:35];
    assign c = cap[110:47];
    assign d = cap[111];
    always_ff @(posedge clk) acc <= {acc[30:0], acc[31]} ^ a[31:0] ^ {20'd0, b} ^ c[63:32] ^ c[31:0] ^ {31'd0, d};
    initial begin
        seed = 32'h9e3779b9;
        acc = 0;
        u.cap = 112'd0;
        u.cap[90] = 1'bx;
        #1 $display("x: a=%h b=%h c=%h d=%b", a, b, c, d);
        u.cap = 112'h1234_5678_9abc_def0_1357_9bdf_2468;
        repeat (30) @(posedge clk) seed = seed * 32'd1664525 + 32'd1013904223;
        #1 $display("a=%h b=%h c=%h d=%b acc=%h", a, b, c, d, acc);
        $finish;
    end
endmodule
