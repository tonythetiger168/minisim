// IEEE 1800-2023 11.4.11: only the arm of ?: that the condition picks is
// evaluated, so a side effect in the other arm (a queue pop, an increment)
// does not happen, and a pop in the picked arm happens after the
// condition is tested, not before.
//! icarus-skip: Icarus 13 does not support queue methods in expressions
module t;
  int q[$];
  int r, i;
  function automatic int rd(); return q.size() ? q.pop_front() : -1; endfunction
  initial begin
    q.push_back(5);
    r = q.size() ? q.pop_front() : -1; $display("%0d %0d", r, q.size());
    q.push_back(6);
    r = rd(); $display("%0d %0d", r, q.size());
    r = rd(); $display("%0d", r);
    q.push_back(7);
    r = (q.size() > 0 && q.pop_back() == 7) ? 1 : 0; $display("%0d %0d", r, q.size());
    r = (q.size() > 0 && q.pop_back() == 7) ? 1 : 0; $display("%0d", r);
    i = 3;
    r = (i > 5) ? i++ : i; $display("%0d %0d", r, i);
    r = (i < 5) ? i++ : i; $display("%0d %0d", r, i);
  end
endmodule
//> 5 0
//> 6 0
//> -1
//> 1 0
//> 0
//> 3 3
//> 3 4
