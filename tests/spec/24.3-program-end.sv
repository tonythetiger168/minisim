// IEEE 1800-2023 24.3: when every initial procedure of a program has ended,
// the threads they forked are killed. When every program has ended, the
// run ends with an implicit $finish, even though the design's clock runs
// forever. Final procedures still run (9.2.3).
//! icarus-skip: Icarus 13 does not kill a program's forked threads or end the run when every program ends (24.3)
module top;
  logic clk = 0;
  always #5 clk = ~clk;
  p1 a();
  p2 b();
  final $display("%0t final", $time);
endmodule

program p1;
  initial begin
    fork
      forever #3 $display("%0t p1 child", $time);
    join_none
    #7 $display("%0t p1 done", $time);
  end
endprogram

program p2;
  initial #12 $display("%0t p2 first done", $time);
  initial begin
    #20 $display("%0t p2 second done", $time);
  end
endprogram

//> 3 p1 child
//> 6 p1 child
//> 7 p1 done
//> 12 p2 first done
//> 20 p2 second done
//> 20 final
