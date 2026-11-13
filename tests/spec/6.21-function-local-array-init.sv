// IEEE 1800-2023 6.21 Scope and lifetime, 10.9.1 array assignment patterns:
// an array declared with an initializer in an automatic function gets its
// values on every call (a lookup table inside an encoder function).
//! icarus-skip: an msim regression (the function keeps static signals); not checked on Icarus
module t;
  function automatic bit [5:0] look(bit [1:0] d, bit inv);
    bit [5:0] t6[4] = '{6'b100111, 6'b011101, 6'b101101, 6'b110001};
    bit [5:0] r;
    r = t6[d];
    if (inv) begin
      t6[d] = ~t6[d];   // changes only this call's copy
      r = t6[d];
    end
    return r;
  endfunction
  initial begin
    $display("%b", look(1, 0));
    $display("%b", look(1, 1));
    $display("%b", look(1, 0));
    $display("%b", look(3, 0));
  end
endmodule
//> 011101
//> 100010
//> 011101
//> 110001
