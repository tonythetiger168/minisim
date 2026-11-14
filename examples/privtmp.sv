// A clocked block with automatic locals (decoder temporaries) in msim
// -compile's cycle domain: the locals become private to the domain (no
// write through the kernel). One local is read before it is written in a
// cycle, so it must keep its value from the previous cycle; another holds
// X for a while. The interpreter and -compile must print the same.
module privtmp;
    reg clk = 0;
    reg rst = 1;
    reg [15:0] insn;
    reg [31:0] imm, acc;
    reg [7:0] seen;
    always #5 clk = ~clk;
    always @(posedge clk) begin : dec
        reg [11:0] lo;
        reg [19:0] hi;
        reg [7:0] last;
        lo = {insn[12], insn[6:5], insn[2], insn[11:10], insn[4:3], 4'b0};
        hi = {{8{insn[15]}}, insn[11:0]};
        seen <= last;          // the previous cycle's value
        last = insn[7:0] ^ lo[7:0];
        if (rst) begin
            imm <= 0;
            acc <= 0;
        end else begin
            imm <= {hi, lo};
            acc <= {acc[30:0], acc[31]} ^ {hi, lo} ^ {24'd0, last};
        end
    end
    always @(posedge clk) insn <= rst ? 16'hxxxx : insn * 16'd25173 + 16'd13849 + (insn == 16'hxxxx ? 16'd0 : 16'd0);
    initial begin
        insn = 16'h1234;
        repeat (3) @(posedge clk);
        #1 $display("x phase: imm=%h acc=%h seen=%h", imm, acc, seen);
        rst = 0;
        insn = 16'h1234;
        repeat (40) @(posedge clk);
        #1 $display("imm=%h acc=%h seen=%h", imm, acc, seen);
        $finish;
    end
endmodule
