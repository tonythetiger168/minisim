// IEEE 1800-2023 7.6: a fixed-size unpacked array may be assigned from a
// dynamic array or a queue of the same element type. The elements go left to
// right; a source of another size is a run-time error and nothing is written.
//! icarus-skip: Icarus 13 does not assign containers to fixed-size arrays
//> '{10, 20, 30} 10
//> '{3, 2, 1}
//> Error: assignment to a fixed-size array of 3 elements from a container of another size
//> '{3, 2, 1}
module top;
  int asc[1:3];
  int desc[2:0];
  int q[$] = '{10, 20, 30};
  int d[];
  initial begin
    asc = q;
    $display("%p %0d", asc, asc[1]);
    d = new[3];
    foreach (d[i]) d[i] = 3 - i;
    desc = d;
    $display("%p", desc);
    q.pop_back();
    desc = q;
    $display("%p", desc);
  end
endmodule
