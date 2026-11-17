// IEEE 1800-2023 18.7: in obj.randomize() with {...} called from a method,
// a name the object does not have is the caller's: also the caller's
// fixed-size arrays, dynamic arrays and queues, read as state (their size,
// an element, inside, a reduction).
//! icarus-skip: Icarus 13 has no constraints
class Item;
  rand int v, n;
endclass
class Gen;
  int lim[3] = '{10, 20, 30};
  int pool[] = '{4, 9, 16};
  int hist[$] = '{1, 2, 3, 4};
  Item it = new;
  function int run();
    int ok = 1;
    ok &= it.randomize() with { v inside {pool}; n == hist.size() + lim[2]; };
    if (!(it.v inside {4, 9, 16}) || it.n != 34) ok = 0;
    ok &= it.randomize() with { if (pool.size() > 2) { v == hist.sum(); } n < lim[0]; n >= 0; };
    if (it.v != 10 || it.n >= 10) ok = 0;
    return ok;
  endfunction
endclass
module top;
  Gen g = new;
  initial begin
    automatic int ok = 1;
    repeat (10) ok &= g.run();
    $display("ok=%0d", ok);
  end
endmodule
//> ok=1
