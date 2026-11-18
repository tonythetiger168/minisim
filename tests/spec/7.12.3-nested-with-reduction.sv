// 7.12.3, 7.12.4: a reduction of a multidimensional fixed-size array whose
// with clause reduces each item (a row) again; item.index is the row's index.
// 20.17.2: $stacktrace called as a function returns a string.
//! icarus-skip: Icarus does not support array reduction methods with a with clause
module top;
  logic [7:0] m[2][2];
  int g[1:2][3];
  int i;
  string trace;
  initial begin
    m = '{'{5, 10}, '{15, 20}};
    i = m.sum with (item.sum with (item));
    $display("%0d", i);
    g = '{'{1, 2, 3}, '{4, 5, 6}};
    i = g.sum with (item.product with (item));
    $display("%0d", i);
    i = g.sum with (item.index * item.sum with (item));
    $display("%0d", i);
    trace = $stacktrace();
    $display("%0d", trace != "");
  end
endmodule
//> 50
//> 126
//> 36
//> 1
