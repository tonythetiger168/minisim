// IEEE 1800-2023 6.24.3: a bit-stream cast converts between an integral
// type and a dynamic array or queue of integral elements; the first element
// holds the leftmost bits, as with a streaming concatenation.
module t;
  typedef byte bytes_t[];
  typedef bit [7:0] q_t[$];
  typedef logic [23:0] word_t;
  function automatic bytes_t bump(bytes_t a);
    foreach (a[i]) a[i] = a[i] + 1;
    return a;
  endfunction
  bytes_t d;
  q_t q;
  word_t w;
  initial begin
    d = '{8'h11, 8'h22, 8'h33};
    w = word_t'(d);
    $display("%h", w);
    w = word_t'(bump(d));
    $display("%h", w);
    d = bytes_t'(24'habcdef);
    $display("%0d %h %h %h", d.size(), d[0], d[1], d[2]);
    q = q_t'(w);
    $display("%0d %h", q.size(), q[$]);
  end
endmodule
//> 112233
//> 122334
//> 3 ab cd ef
//> 3 34
