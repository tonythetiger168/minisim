// IEEE 1800-2023 9.2.3: $finish in a final procedure ends the simulation
// at once: the rest of that procedure and the final procedures not yet run
// do not execute. (Their order is arbitrary, so both print the same line:
// exactly one copy appears.)
module t;
  sub u();
  initial #1;
  final begin
    $display("final ran");
    $finish(0);
    $display("after $finish");
  end
endmodule
module sub;
  final begin
    $display("final ran");
    $finish(0);
  end
endmodule
//> final ran
