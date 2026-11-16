// IEEE 1800-2023 6.21: a static variable declared in a block of a
// procedure is initialized once, before any procedure starts, even when
// that block never runs, and a later entry into the block does not
// initialize it again.
//! icarus-skip: Icarus 13 leaves a static in a block that never runs at 0
module top;
  // declared first, so msim starts it first (9.2.1 leaves the order open)
  initial begin
    $display("%0d %0d %0d", a.dead.x, b.body.n, b.body.arr[0]);
    #1 $display("%0d %0d %0d", a.dead.x, b.body.n, b.body.arr[0]);
  end
  initial begin : a
    if (0) begin : dead
      static int x = 7;
      x = 70;
    end
  end
  initial begin : b
    repeat (3) begin : body
      static int n = 10;
      static int arr[2] = '{4, 5};
      n = n + 1;
      arr[0] = arr[0] + 1;
    end
  end
endmodule
//> 7 10 4
//> 7 13 7
