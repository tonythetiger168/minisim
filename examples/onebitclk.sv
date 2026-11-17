// One-bit procedural writes (a testbench clock, a flag): msim -compile
// emits S.setOneBit, the direct edge, when the value is a known 0 or 1 and
// only clocked or comb logic watches the signal; anything else goes
// through set(). Same output as the interpreter, and as with MSIM_NOSETBIT.
module onebitclk;
  logic clk;              // x until #2: the first writes go through set()
  logic en = 1, flag;
  logic [7:0] cnt = 0, cnt2 = 0, waits = 0;
  wire gclk = clk & en;
  always #5 clk = ~clk;
  initial #2 clk = 0;
  always_ff @(posedge clk) cnt <= cnt + 8'd1;
  always_ff @(posedge gclk) cnt2 <= cnt2 + {7'd0, flag === 1'b1};
  initial begin
    flag = 1'bx;            // an x: set()
    repeat (3) @(posedge clk) waits = waits + 1;   // a waiter on the clock
    flag = 1;
    #40 en = 0;
    #30 en = 1;
    flag = 0;
    #20 flag = 1;
    #50 $display("cnt=%0d cnt2=%0d waits=%0d flag=%b", cnt, cnt2, waits, flag);
    $finish;
  end
endmodule
