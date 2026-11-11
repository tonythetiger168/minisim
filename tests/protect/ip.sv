// Protected IP for tests/protect_conformance.sh (IEEE 1735-2023 5.2, 9.3,
// 9.4, 9.9, Annex A).
`pragma protect begin
`pragma protect viewport ip_core.open_a, ip_core:blk:open_b
module ip_core(input logic clk, output logic [7:0] q);
  logic [7:0] hidden = 8'h21;
  logic [7:0] open_a = 8'h0a;
  if (1) begin : blk
    logic [7:0] open_b = 8'h0b;
  end
  logic [7:0] fifo [$];
  ip_child kid();
  class token;
    int v;
    function new(int x); v = x; endfunction
    function int get(); return v; endfunction
  endclass
  // A dynamic object handed to the user (9.9): it has no name in the
  // design, so it stays usable outside the IP.
  function token make(int x);
    token t;
    t = new(x);
    return t;
  endfunction
  always_ff @(posedge clk) begin
    fifo.push_back(hidden);
    q <= hidden + fifo.size();
  end
endmodule
`pragma protect end

// Clear text, but only ever instantiated inside the protected module.
module ip_child;
  logic [3:0] kid_sig = 4'h5;
endmodule
