// 13.5.2: a ref argument whose actual is an element of a queue, dynamic
// array or associative array. The callee's writes reach the element; once
// the element is gone (an outdated reference), they no longer do.
//! icarus-skip: Icarus does not support ref arguments to container elements
module top;
  int q[$] = '{1, 2, 3};
  string names[string];
  int d[];
  task automatic bump(ref int e, input int by); e += by; endtask
  function automatic void excl(ref string s); s = {s, "!"}; endfunction
  task automatic slow(ref int e);
    #100 e = 10;
  endtask
  initial begin
    d = new[2];
    bump(q[1], 5);
    bump(d[1], 7);
    names["a"] = "hi";
    excl(names["a"]);
    $display("%p %p %p", q, d, names);
    fork slow(q[2]); join_none
    #50 $display("%0d", q[2]);
    #100 $display("%p", q);
    fork slow(q[0]); join_none
    #50 q.delete();
    #100 $display("%0d", q.size());
  end
endmodule
//> '{1, 7, 3} '{0, 7} '{"a":"hi!"}
//> 3
//> '{1, 7, 10}
//> 0
