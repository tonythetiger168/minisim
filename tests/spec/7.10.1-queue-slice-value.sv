// 7.10.1: a queue slice q[a:b] is a queue value of the elements a..b. Used
// directly as a value (here printed with %p, 21.2.1.6) with bounds that
// change at run time; bounds past the end are clipped, and a > b is empty.
//! icarus-skip: Icarus does not print a queue slice with %p
module top;
  int i;
  int q[$] = '{10, 11, 12};
  initial begin
    for (i = 0; i < 3; i++) begin
      $display("%p", q[i:i+1]);
      q.push_back(13 + i);
    end
    i = 4;
    $display("%p %p", q[i:i+9], q[2:1]);
    $display("%0d", q[1:3].sum());
  end
endmodule
//> '{10, 11}
//> '{11, 12}
//> '{12, 13}
//> '{14, 15} '{}
//> 36
