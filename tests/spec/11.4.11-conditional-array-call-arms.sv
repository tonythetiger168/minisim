// IEEE 1800-2023 11.4.11: on unpacked arrays too, only the arm the condition
// picks is evaluated, so a function call in the other arm does not run (a
// recursive function ends); an x condition runs both and merges them.
//! icarus-skip: Icarus does not support ?: on unpacked array values
module t;
  typedef int arr_t[3];
  int calls;
  function automatic arr_t fill(int depth);
    arr_t r = depth > 1 ? fill(depth - 1) : '{depth, depth * 2, depth * 3};
    calls++;
    return r;
  endfunction
  function automatic arr_t mk(int v);
    calls++;
    return '{v, 2, v};
  endfunction
  initial begin
    arr_t a;
    logic c;
    a = fill(4);
    $display("%p %0d", a, calls);
    calls = 0; c = 1; a = c ? mk(1) : mk(3);
    $display("%p %0d", a, calls);
    calls = 0; c = 1'bx; a = c ? mk(1) : mk(3);
    $display("%p %0d", a, calls);
  end
endmodule
//> '{1, 2, 3} 4
//> '{1, 2, 1} 1
//> '{0, 2, 0} 2
