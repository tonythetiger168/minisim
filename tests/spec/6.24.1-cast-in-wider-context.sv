// IEEE 1800-2023 6.24.1, 11.6.1, 11.8.2: a size or type cast is a boundary
// for expression width. The operand is worked out at its own size, then the
// result is cut (or kept) at the cast's size, and only that result is
// extended by a wider context (a compare with an int, a 32-bit sum, an
// index).
module top;
  logic [15:0] a = 16'hFFFC;
  int k = 5;
  logic [7:0] b = 8'hF0, c = 8'h20;
  logic [7:0] mem [0:7];
  int s;
  initial begin
    foreach (mem[i]) mem[i] = 8'(i * 3);
    $display("wrap %0d %b", 16'(a + k), 16'(a + k) < 32);
    $display("same %b", 16'(a + 16'(k)) < 32);
    $display("byte %0d %0d", 8'(b + c) + 0, (b + c) + 0);
    s = int'(8'(b + c)) - 32'd20;
    $display("sum %0d", s);
    $display("index %0d", mem[3'(a + k)]);
    $display("shift %h", 16'((a + 32'd4) >> 1));
    $display("signed %0d", 4'(-8'sd3) + 0);
  end
endmodule
//> wrap 1 1
//> same 1
//> byte 16 272
//> sum -4
//> index 3
//> shift 8000
//> signed -3
