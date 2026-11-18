// 9.4.5: blocking assignments with an intra-assignment delay or event to an
// element of a container, and to string variables. The value and the element
// are both taken when the statement starts; the write happens after the wait.
//! icarus-skip: Icarus does not support associative arrays of strings
module top;
  int q[$] = '{1, 2, 3};
  string aa[int];
  int byName[string];
  string s;
  int k = 6;
  initial begin
    aa[k] = #2 "y";
    k = 99;
    $display("%0t %p", $time, aa);
    q[1] = #1 q[0] + 10;
    s = #1 aa[6];
    byName["w"] = @(k) 5;
    $display("%0t %p %p %s %p", $time, aa, q, s, byName);
  end
  initial #6 k = 1;
endmodule
//> 2 '{6:"y"}
//> 6 '{6:"y"} '{1, 11, 3} y '{"w":5}
