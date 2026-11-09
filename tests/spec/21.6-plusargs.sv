// IEEE 1800-2023 21.6: $test$plusargs and $value$plusargs with no plusargs
// given (21.6).
//! xfail: plusargs are errors
module top;
  int v = 5;
  initial begin
    $display("%0d", $test$plusargs("verbose"));
    $display("%0d %0d", $value$plusargs("N=%d", v), v);
  end
endmodule

//> 0
//> 0 5
