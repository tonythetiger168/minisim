//! icarus-skip: Icarus does not parse a locator method as a statement
// IEEE 1800-2023 7.12.1, 13.4.1: a locator method called as a statement.
// Its result is discarded, but the with clause is still evaluated for
// each element, in order.
module top;
  int q[$] = '{3, 1, 4, 1, 5};
  int seen[$];
  function automatic bit note(int v);
    seen.push_back(v);
    return v > 2;
  endfunction
  initial begin
    q.find with (note(item));
    $display("%p", seen);
    q.find with (item == 4);
    $display("done");
  end
endmodule
//> '{3, 1, 4, 1, 5}
//> done
