// IEEE 1800-2023 19.3: a covergroup's block event may name a named block:
// @@(begin blk) samples when the block starts, @@(end blk) when it ends,
// each time it runs (here inside a task called twice).
//! icarus-skip: Icarus has no covergroups
module t;
  int v;
  covergroup cg @@(end t.step.work or begin t.init_blk.body);
    coverpoint v { bins lo = {[0:4]}; bins mid = {[5:6]}; bins hi = {[7:9]}; }
  endgroup
  cg c = new;
  task automatic step(int x);
    begin : work
      v = x;
    end
    v = 100;
  endtask
  initial begin : init_blk
    step(2);
    $display("%.2f", c.get_coverage());
    step(8);
    $display("%.2f", c.get_coverage());
    v = 5;
    begin : body   // sampled as it starts: v is 5
      v = 0;
    end
    $display("%.2f", c.get_coverage());
  end
endmodule
//> 33.33
//> 66.67
//> 100.00
