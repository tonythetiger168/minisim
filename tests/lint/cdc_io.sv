// Port domains from the SDC file: set_input_delay / set_output_delay -clock.
module cdc_io(input logic clk, din_a, din_v, din_s, output logic dout_a, dout_v);
  // din_a is launched by clk (the same clock): no crossing.
  logic qa;
  always_ff @(posedge clk) qa <= din_a;
  assign dout_a = qa;
  // din_v comes from the virtual clock VIO, read by one flop of clk: CDC001.
  logic qv;
  always_ff @(posedge clk) qv <= din_v;
  // din_s from VIO through two flops: synchronized.
  logic s1, s2;
  always_ff @(posedge clk) begin s1 <= din_s; s2 <= s1; end
  // dout_v is captured by VIO but launched by clk: CDC001 at the port.
  always_ff @(posedge clk) dout_v <= qv ^ s2;
endmodule
