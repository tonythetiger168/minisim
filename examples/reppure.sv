// Replications of an operator, {32{a & b[31]}} as in an arithmetic shift's
// fill: msim -compile computes the repeated operand once, in 2-state and
// 4-state code, and must match the interpreter, x bits included.
module reppure;
  logic clk = 0;
  logic arith = 1;
  logic en = 1;          // set to x at the end
  logic [31:0] op = 32'h8000_1234;
  logic [31:0] fill, fill2;
  logic [15:0] mix;
  logic [3:0] nib = 4'b1010;
  logic [63:0] acc = 0;
  always_comb fill = {32{arith & op[31] & en}};
  assign fill2 = {16{nib[1:0] ^ op[1:0]}};
  assign mix = {{4{~nib[3]}}, {6{(arith & en) | op[0]}}, {3{op[2] ^ op[3]}}, {3{nib == 4'd10}}};
  always_ff @(posedge clk) begin
    op <= {op[30:0], op[31] ^ op[7]} + 32'h135;
    arith <= ~arith ^ op[4];
    nib <= nib + 4'd3;
    acc <= acc * 64'd3 + {fill, fill2} + {48'd0, mix};
  end
  initial begin
    repeat (50) begin #1 clk = 1; #1 clk = 0; end
    $display("acc=%h fill=%h fill2=%h mix=%h", acc, fill, fill2, mix);
    en = 1'bx;
    #1 $display("x fill=%b mix=%b", fill, mix);
    $finish;
  end
endmodule
