// IEEE 1800-2023 16.8.2: a sequence may take a local variable argument with
// direction output or inout. The actual is a local variable of the
// instantiating property; what the sequence assigns to the formal flows out
// to it, and an inout formal starts with the actual's value.
//! icarus-skip: Icarus has no concurrent assertions
module t;
  bit clk;
  logic [7:0] d;
  bit start, done;
  logic [7:0] dout;
  always #5 clk = !clk;
  sequence s_get(local output logic [7:0] v);
    start ##0 (1, v = d);
  endsequence
  sequence s_inc(local inout logic [7:0] v);
    (1, v = v + 1);
  endsequence
  property p_check;
    logic [7:0] x;
    @(posedge clk) s_get(x) ##0 s_inc(x) |-> ##2 done && dout == x;
  endproperty
  a_check: assert property (p_check) else $display("%0t fail dout=%0d", $time, dout);
  c_check: cover property (@(posedge clk) start ##2 done) $display("%0t done", $time);
  initial begin
    @(negedge clk) start = 1; d = 10;
    @(negedge clk) start = 0;
    @(negedge clk) done = 1; dout = 11;
    @(negedge clk) done = 0; start = 1; d = 20;
    @(negedge clk) start = 0;
    @(negedge clk) done = 1; dout = 20;   // should be 21
    @(negedge clk) done = 0;
    #10 $finish;
  end
endmodule
//> 35 done
//> 65 fail dout=20
//> 65 done
