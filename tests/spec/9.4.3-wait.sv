// IEEE 1800-2017 9.4.3 Level-sensitive event control: wait (expr) blocks until
// expr is true; if it is already true, the statement runs at once.
module t;
  logic r = 0;
  initial #3 r = 1;
  initial begin
    wait (r) $display("r high at %0t", $time);
    wait (r) $display("again at %0t", $time);
  end
endmodule
//> r high at 3
//> again at 3
