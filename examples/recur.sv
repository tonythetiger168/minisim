// A recursive function is rejected. Static storage cannot represent it.
module recur;
  function automatic logic [7:0] fact(input logic [7:0] n);
    if (n == 0) return 8'h01;
    return n * fact(n - 8'h01);
  endfunction
  initial $display("%b", fact(8'h03));
endmodule
