// Protected IP that uses the programming language interfaces itself
// (IEEE 1735-2023 9.7): a user system task and function, and a DPI import.
// `seen` is made visible with a viewport; everything else is protected.
`pragma protect begin
module secret_pli(input logic clk, input logic [7:0] d, output logic [7:0] q);
  import "DPI-C" context function void dpi_where(input int tag);
  `pragma protect viewport
  logic [7:0] seen = 8'h11;
  logic [7:0] hidden = 8'h22;
  logic [7:0] mem [0:3];
  parameter int SECRET_P = 7;
  clear_child c();
  always_ff @(posedge clk) q <= d + hidden;
  initial begin
    mem[2] = 8'h5a;
    #1 $ip_task(hidden, seen, mem[2], 8'd9 + hidden);
    hidden = $ip_func(hidden);
    dpi_where(1);
    $display("ip: %m hidden=%h", hidden);
    #20 $display("ip: hidden=%h after the probe", hidden);
  end
endmodule
`pragma protect end

// Clear text, but instantiated inside the protected module.
module clear_child;
  logic [3:0] cc = 4'h3;
endmodule
