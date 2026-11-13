// IEEE 1800-2023 37.35 (expressions) and 38.15: vpi_get_value on an
// expression object gives its value now, not only for a constant
// expression: the right-hand sides of continuous assignments and their
// operands, selects of vectors and memories, a conditional, a concatenation,
// a real expression and a function call, read at two times. An expression
// that reads an automatic variable has no value here (an error).
//! icarus-skip: Icarus 13 gives no value for operations through VPI
//! args: data/vpi/exprval.c
module top;
  logic [7:0] a = 8'd3, b = 8'd4, mem [0:3];
  logic [3:0] sel = 4'b0101;
  logic c = 0;
  real r = 1.5;
  wire [7:0] s1;
  assign s1 = (a + b) * 2;
  wire [7:0] s2;
  assign s2 = c ? mem[sel[1:0]] : {sel, a[3:0]};
  wire [7:0] s3;
  assign s3 = twice(b) - 1;
  wire       s4;
  assign s4 = r > 2.0;
  wire [7:0] s5;
  assign s5 = a + 8'bx;
  function automatic logic [7:0] twice(logic [7:0] x); return x << 1; endfunction
  task automatic t(input int k);
    int j;
    j = k + 1;
  endtask
  initial begin
    mem[0] = 8'h10; mem[1] = 8'h11; mem[2] = 8'h12; mem[3] = 8'h13;
    #1 $exprs;
    a = 8'd10; c = 1; r = 2.5;
    #1 $exprs;
    t(1);
    $autos;
  end
endmodule
//> s1: 14 = 14 (operands 7 2)
//> s2: 83 = 83 (operands 0 17 83)
//> s3: 7 = 7 (operands 8 1)
//> s4: 0 = 0 (operands 1.5 2)
//> s5: x = x (operands 3 x)
//> s1: 28 = 28 (operands 14 2)
//> s2: 17 = 17 (operands 1 17 90)
//> s3: 7 = 7 (operands 8 1)
//> s4: 1 = 1 (operands 2.5 2)
//> s5: x = x (operands 10 x)
//> automatic: no value, error 1
