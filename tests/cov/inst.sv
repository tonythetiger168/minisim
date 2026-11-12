// Per-instance coverage: two instances of leaf; u1 has b tied low, so its
// else-branch line, one branch outcome and two condition vectors are covered
// only through u0.
module leaf(input logic clk, a, b, output logic y, output logic [1:0] n);
  function automatic logic both(input logic p, q);
    return p && q;
  endfunction
  always_ff @(posedge clk) begin
    if (a && b)
      y <= 1'b1;
    else
      y <= 1'b0;
    n <= both(a, b) ? 2'd3 : 2'd0;
  end
endmodule

module mid(input logic clk, a, output logic y);
  logic [1:0] n;
  leaf u(.clk, .a, .b(1'b0), .y, .n);
endmodule

module tb;
  logic clk = 0, a = 0, b = 0, y0, y1;
  logic [1:0] n0;
  leaf u0(.clk, .a, .b, .y(y0), .n(n0));
  mid  u1(.clk, .a, .y(y1));
  always #5 clk = ~clk;
  initial begin
    repeat (2) @(posedge clk);
    a = 1; @(posedge clk);
    b = 1; @(posedge clk);
    a = 0; @(posedge clk);
    b = 0; @(posedge clk);
    $display("done y0=%b y1=%b", y0, y1);
    $finish;
  end
endmodule
