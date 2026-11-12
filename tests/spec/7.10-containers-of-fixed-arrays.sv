// IEEE 1800-2023 7.4, 7.5, 7.8, 7.10, 13.4, 6.6.7: fixed-size unpacked
// arrays as values. A queue, dynamic array or associative array whose
// elements are fixed-size arrays; a fixed-size array copied into and out of
// a struct member; a function that returns a fixed-size array; and a
// user-defined nettype whose data type is a fixed-size array, resolved by
// a function over the dynamic array of its drivers' values.
//! icarus-skip: Icarus has no unpacked structs, nettypes or queues of arrays
module top;
  typedef logic [3:0] pair_t [2];
  pair_t q[$];
  int d[][3];
  pair_t aa[string];
  pair_t x;
  struct { logic [3:0] f [2]; int k; } s;

  function automatic pair_t swap(pair_t p);
    swap[0] = p[1];
    swap[1] = p[0];
  endfunction
  function automatic pair_t orall(input pair_t v []);
    orall = '{default: 0};
    foreach (v[i]) begin orall[0] |= v[i][0]; orall[1] |= v[i][1]; end
  endfunction
  nettype pair_t pnet with orall;
  pnet n;
  assign n = '{4'h1, 4'h2};
  assign n = '{4'h4, 4'h8};

  initial begin
    $display("%p", s);
    x = '{4'h1, 4'h2};
    q.push_back(x);
    q.push_back('{4'h3, 4'h4});
    $display("%0d %h %h %h", q.size(), q[0][0], q[0][1], q[1][1]);
    q[1][0] = 4'h9;
    x = q[1];
    $display("%h %h", x[0], x[1]);
    d = new[2];
    d[1][2] = 7;
    $display("%p", d);
    foreach (d[i, j]) if (d[i][j]) $display("d[%0d][%0d]=%0d", i, j, d[i][j]);
    aa["k"] = '{4'ha, 4'hb};
    $display("%h", aa["k"][1]);
    $display("%p", q);
    s.f = x;
    s.f[1] = 4'h7;
    x = s.f;
    $display("%h %h", x[0], x[1]);
    x = swap(x);
    $display("%h %h", x[0], x[1]);
    #1 $display("%h %h", n[0], n[1]);
  end
endmodule
//> '{f:'{x, x}, k:0}
//> 2 1 2 4
//> 9 4
//> '{'{0, 0, 0}, '{0, 0, 7}}
//> d[1][2]=7
//> b
//> '{'{1, 2}, '{9, 4}}
//> 9 7
//> 7 9
//> 5 a
