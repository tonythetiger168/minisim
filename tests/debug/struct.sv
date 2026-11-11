// mdebug self-test: struct fields. A struct is one vector in the waveform;
// each field has its own driver, and a condition can read a field.
module st;
  typedef struct packed { logic [3:0] hi; logic [3:0] lo; } pair_t;
  typedef struct { logic [3:0] a; logic b; } rec_t;
  logic clk = 0;
  pair_t p; rec_t r;
  logic [1:0] idx = 0;
  logic [7:0] q = 0, s; logic [3:0] t, u;
  always #5 clk = ~clk;
  always @(posedge clk) begin idx <= idx + 1; q <= q + 8'd4; end
  always_comb begin p = '0; p.hi = q[3:0]; p.lo = 4'd3; end
  always_comb if (p.lo == 4'd3) s = {p.hi, p.lo}; else s = 0;
  always_comb begin r.a = p.hi; r.b = idx[0]; end
  always_comb t = r.b ? r.a : 4'd0;
  always_comb u = p.hi + 4'd1;   // reads one field: only its driver is a cause
  initial begin #60 $display("s=%0h t=%0d", s, t); $finish; end
endmodule
