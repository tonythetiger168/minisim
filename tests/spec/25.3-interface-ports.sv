// IEEE 1800-2023 25.3, 25.5.4, 20.6.2: a parameterized interface, a generic
// interface port (cnt_if c), a net with a continuous assignment inside the
// interface, a modport import of an interface function, and $bits.
//! icarus-skip: Icarus 13 does not support interface ports
interface cnt_if #(parameter int W = 4) (input logic clk);
  logic [W-1:0] q;
  logic en;
  wire [W-1:0] next = q + 1;
  function automatic logic [W-1:0] twice();
    return q * 2;
  endfunction
  modport ctl(output en, import twice);
endinterface

module counter(cnt_if c);
  initial c.q = 0;
  always @(posedge c.clk) if (c.en) c.q <= c.next;
endmodule

module driver(cnt_if.ctl c);
  initial begin
    c.en = 1;
    #42 c.en = 0;
    $display("twice %0d", c.twice());
  end
endmodule

module top;
  logic clk = 0;
  always #5 clk = ~clk;
  cnt_if #(.W(4)) ci(clk);
  counter u(ci);
  driver d(ci);
  initial #100 begin $display("q=%0d w=%0d", ci.q, $bits(ci.q)); $finish; end
endmodule

//> twice 8
//> q=4 w=4
