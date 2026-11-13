// IEEE 1800-2023 14.3, 24.3: a clocking block declared in a program, used by
// the program's own initial block.
//! icarus-skip: Icarus 13 has no clocking blocks
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  logic [3:0] cnt = 0;
  always @(posedge clk) cnt <= cnt + 1;
  logic [3:0] o;
  p u(clk, cnt, o);
  always @(posedge clk) if ($time > 10) $display("%0t o=%0d", $time, o);
endmodule
program p(input logic clk, input logic [3:0] c, output logic [3:0] o);
  clocking pc @(posedge clk);
    input c;
    output o;
  endclocking
  initial begin
    repeat (2) @(pc);
    $display("%0t c=%0d", $time, pc.c);
    pc.o <= pc.c + 4'd1;
    repeat (2) @(pc);
    $display("%0t c=%0d", $time, pc.c);
    $finish;
  end
endprogram

//> 15 o=x
//> 15 c=1
//> 25 o=2
//> 35 o=2
//> 35 c=3
