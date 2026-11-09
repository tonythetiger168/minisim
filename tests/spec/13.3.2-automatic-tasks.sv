// IEEE 1800-2023 13.3.2, 6.21: Each call of an automatic task has its own
// arguments and locals, so two calls that overlap in time do not share them.
// An automatic variable with no initializer starts at its default on every
// call (6.21).
module top;
  task automatic work(input int id, input int d, output int r);
    int v;
    v = v + id * 10;   // v is 0 on entry: no value left from another call
    #d;
    r = v + d;
    $display("%0t: id %0d r %0d", $time, id, r);
  endtask
  int r1, r2;
  initial begin
    fork
      work(1, 3, r1);
      work(2, 1, r2);
    join
    $display("%0d %0d", r1, r2);
  end
endmodule

//> 1: id 2 r 21
//> 3: id 1 r 13
//> 13 21
