// -compile stores some NBAs at once instead of through a commit slot
// (codegen.hpp, direct NBAs). The cases around that rule, each against
// the interpreter: read before the write (direct), read after a write on
// one path only (not direct), two writes in one pass, a register another
// process reads, part-select NBAs of a narrow register.
module directnba;
  logic clk = 0;
  logic [7:0] a = 0, b = 0, c = 0, d = 0, q = 0, seen = 0;
  logic [15:0] pc = 16'h1234;
  logic [1:0] mode = 0;
  always #5 clk = ~clk;
  always @(posedge clk) begin
    mode <= mode + 1;
    seen <= a;                 // read before the write below
    a <= a + 1;
    if (mode == 1) b <= b + 3;
    else c <= b;               // reads b; the write is on the other path
    if (mode[1]) begin
      d <= d + 1;
      d <= d + 2;              // the last one wins
    end
    if (mode == 3) c <= c ^ d;
    pc[0] <= 0;
    pc[7:4] <= pc[7:4] + 1;
  end
  always @(posedge clk) q <= c ^ 8'h5a;   // another process reads c
  initial begin
    repeat (8) @(negedge clk)
      $display("c=%h q=%h seen=%h pc=%h", c, q, seen, pc);   // a, b and d only in their process
    $finish;
  end
endmodule
