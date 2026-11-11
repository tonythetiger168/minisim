// The IP user's side for tests/protect_conformance.sh.
module spy(input logic [7:0] watched);
  logic [7:0] copy;
  assign copy = watched;
endmodule

module tb;
  logic clk = 0;
  wire [7:0] q;
  ip_core u(.clk(clk), .q(q));
  // A module bound into the protected IP (9.4.2.2): the IP's visibility
  // rules hold for it.
  bind ip_core spy s(.watched(hidden));
  always #5 clk = ~clk;
  initial begin
    // 9.3.2: in the build phase protected names can be referenced from any
    // HDL code; the model behaves the same protected or not (9.2.2).
    $display("tb: hidden=%h open_a=%h open_b=%h", u.hidden, u.open_a, u.blk.open_b);
    $display("tb: token=%0d", u.make(42).get());
    #22 $display("tb: q=%h fifo=%0d", q, u.fifo.size());
    $finish;
  end
endmodule
