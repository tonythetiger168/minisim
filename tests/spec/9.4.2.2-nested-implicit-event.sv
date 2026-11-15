// 9.4.2.2: @* adds the signals its own statement reads. A nested @* waits
// only on what its statement reads, not on the rest of the process.
module top;
  reg a, b, c;
  always @* begin   // waits on b and c
    a = b;
    $display("%0t outer", $time);
    @* a = c;       // waits on c only
    $display("%0t inner", $time);
  end
  initial begin
    #10 b = 0;
    #10 b = 1;      // the inner @* does not wake
    #10 c = 0;
    #10 c = 1;
  end
endmodule
//> 10 outer
//> 30 inner
//> 40 outer
