// msim -xprop: disable of a named block from inside it (9.6.2), in a loop
// and in a plain branch. Under an unknown condition the rest of the block
// merges with not running it, as for break.
//! args: -xprop
module top;
  logic [7:0] req; logic [3:0] n, m; logic a;
  always_comb begin
    n = 0;
    begin : scan
      for (int i = 0; i < 8; i++) begin
        if (req[i]) disable scan;
        n = n + 1;
      end
    end
  end
  always_comb begin
    m = 4'd1;
    begin : blk
      if (a) disable blk;
      m = 4'd2;
    end
    m = m + 4'd4;
  end
  initial begin
    req = 8'h10; a = 0; #1 $display("n=%0d m=%0d", n, m);
    req = 8'b000x_0000; a = 1'bx; #1 $display("n=%b m=%b", n, m);
    req = 8'b0001_0x00; a = 1; #1 $display("n=%b m=%b", n, m);
    req = 0; #1 $display("n=%b m=%b", n, m);
    $finish;
  end
endmodule
//> n=4 m=6
//> n=xxxx m=xxxx
//> n=xxxx m=0101
//> n=1000 m=0101
