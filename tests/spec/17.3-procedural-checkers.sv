// IEEE 1800-2023 17.3 / 17.4: checkers instantiated in procedural code.
// Everything in a procedural checker instance other than its static
// assertions runs on its own (sum is updated every clock, visited or not).
// Its static concurrent assertions are queued as procedural concurrent
// assertions each time the process reaches the instance, with the
// procedure's clock and the automatic values of that moment (three
// instances of check_loop, one per visited loop iteration, comparing with
// arr[5], arr[10] and arr[20]); its deferred assertions report like
// procedural deferred assertions at the instantiation point. $inferred_clock
// in a procedure is the procedure's clock, and a checker instantiated
// statically in a procedural checker is procedural too (17.3, 16.14.7).
//! icarus-skip: Icarus 13 has no checkers
checker c1(event clk, logic [7:0] a, b);
  logic [7:0] sum = 0;
  always_ff @(clk) sum <= a + 1;
  p0: assert property (@clk sum < 50) else $display("%0t p0 fail %m sum=%0d", $time, sum);
  p1: assert property (@clk a != 3) else $display("%0t p1 fail %m a=%0d", $time, a);
  p2: assert property (@clk a != b) else $display("%0t p2 fail %m a=%0d", $time, a);
  p3: assert final (a != 1) else $display("%0t p3 fail %m", $time);
endchecker
checker c2(logic x, event ck = $inferred_clock);
  px: assert property (@ck x != 3) else $display("%0t px fail %m", $time);
endchecker
checker c3(logic y);
  c2 inner(y);   // static in c3: queued with c3's procedural instance
endchecker
module m;
  bit clk = 0, en = 0;
  logic [7:0] in1 = 0, in2 = 1;
  logic [7:0] arr [0:31];
  always #5 clk = ~clk;
  initial foreach (arr[i]) arr[i] = i;
  c1 check_outside(posedge clk, in1, in2);
  always @(posedge clk) begin
    automatic logic [7:0] v1 = 0;
    if (en) c1 check_inside(posedge clk, in1, v1);
    c3 nested(in1);
    for (int i = 0; i < 4; i++) begin
      v1 = v1 + 5;
      if (i != 2) c1 check_loop(posedge clk, in1, arr[v1]);
    end
  end
  initial begin
    #12 in1 = 3;           // p1 fails in every visited instance
    #10 in1 = 1; en = 1;   // check_inside: b = v1 = 0
    #10 in1 = 0;           // p2 fails in check_inside (a == b == 0)
    #10 in1 = 10;          // check_loop with arr[10]: p2 fails
    #10 in1 = 60;          // p0 fails everywhere a cycle later
    #20 $finish;
  end
endmodule
//> 15 p1 fail m.check_outside.p1 a=3
//> 15 px fail m.nested.inner.px
//> 15 p1 fail m.check_loop.p1 a=3
//> 15 p1 fail m.check_loop.p1 a=3
//> 15 p1 fail m.check_loop.p1 a=3
//> 22 p3 fail m.check_outside.p3
//> 25 p2 fail m.check_outside.p2 a=1
//> 25 p3 fail m.check_inside.p3
//> 25 p3 fail m.check_loop.p3
//> 25 p3 fail m.check_loop.p3
//> 25 p3 fail m.check_loop.p3
//> 35 p2 fail m.check_inside.p2 a=0
//> 45 p2 fail m.check_loop.p2 a=10
//> 65 p0 fail m.check_outside.p0 sum=61
//> 65 p0 fail m.check_inside.p0 sum=61
//> 65 p0 fail m.check_loop.p0 sum=61
//> 65 p0 fail m.check_loop.p0 sum=61
//> 65 p0 fail m.check_loop.p0 sum=61
