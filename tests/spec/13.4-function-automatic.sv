// IEEE 1800-2023 13.4 Functions: return value, automatic lifetime.
module t;
  function automatic int twice(int n);
    return n * 2;
  endfunction
  initial $display("%0d", twice(21));
endmodule
//> 42
