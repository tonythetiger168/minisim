// IEEE 1800-2023 9.3.2 Parallel blocks: fork...join waits for every branch,
// join_any for the first, join_none not at all.
module t;
  initial begin
    fork
      #2 $display("b at %0t", $time);
      #1 $display("a at %0t", $time);
    join
    $display("joined at %0t", $time);
    fork
      #5 $display("slow at %0t", $time);
      #1 $display("fast at %0t", $time);
    join_any
    $display("join_any at %0t", $time);
    fork
      begin #1 $display("none branch at %0t", $time); end
    join_none
    $display("join_none at %0t", $time);
  end
endmodule
//> a at 1
//> b at 2
//> joined at 2
//> fast at 3
//> join_any at 3
//> join_none at 3
//> none branch at 4
//> slow at 7
