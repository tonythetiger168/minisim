// Immediate assertions under -compile (16.3, 16.4, 12.5.3, 20.11): a simple
// assertion with both actions, deferred assertions whose zero-delay glitch is
// flushed, a unique case violation, and $assertoff / $asserton.
module assertions;
  logic [3:0] a, b;
  logic clk = 0;
  int fails = 0;
  always #5 clk = ~clk;

  always @(posedge clk) begin
    chk: assert (a != 4'd3) $display("%0t pass a=%0d", $time, a);
         else begin fails++; $display("%0t fail a=%0d", $time, a); end
  end

  // b glitches through 9 at time 22; the deferred check sees the final value.
  always @(b) dchk: assert #0 (b != 4'd9) else $display("%0t deferred fail b=%0d", $time, b);

  always @(a) begin
    unique case (a)
      4'd1, 4'd2: ;
      4'd5: ;
      4'd7: ;
    endcase
  end

  initial begin
    a = 0; b = 0;
    #12 a = 3;
    #10 b = 9; b = 1;
    #10 b = 9;
    #10 a = 6;
    $assertoff(0, assertions.chk);
    #10 a = 3;
    #10 $asserton(0, assertions.chk);
    #10 $display("fails=%0d", fails);
    $finish;
  end
endmodule
