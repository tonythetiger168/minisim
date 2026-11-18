// 7.12.5: the array mapping method. map() with (...) gives an array with the
// source's indices (or keys), each element the with expression's value for
// that element; item.index is the element's index.
//! icarus-skip: Icarus does not support map()
module top;
  int A[] = '{1, 2, 3}, B[] = '{2, 3, 5}, C[$];
  bit same[];
  int byKey[string] = '{"x": 1, "y": 5};
  int scaled[string];
  string names[$] = '{"ab", "c"};
  int lens[$];
  int F[1:3] = '{1, 2, 3}, G[1:3];
  initial begin
    A = A.map() with (int'(item + 1'b1));
    $display("%p", A);
    C = A.map(a) with (a + B[a.index]);
    $display("%p", C);
    same = A.map(a) with (a == B[a.index]);
    $display("%p", same);
    scaled = byKey.map(v) with (v * 10);
    $display("%p", scaled);
    lens = names.map(n) with (n.len());
    $display("%p", lens);
    C = C.map(x) with (x * x);
    $display("%p", C);
    G = F.map(x) with (x + x.index);
    $display("%p %0d", G, F.map(x) with (x * 2).sum());
  end
endmodule
//> '{2, 3, 4}
//> '{4, 6, 9}
//> '{1, 1, 0}
//> '{"x":10, "y":50}
//> '{2, 1}
//> '{16, 36, 81}
//> '{2, 4, 6} 12
