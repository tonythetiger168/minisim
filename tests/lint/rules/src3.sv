//! args: -only STY013,STY014,STY015,SUS004 -top src3
package src3_pkg;
  parameter int W = 8; //~ STY014
  localparam int D = 4;
endpackage
module src3 #(
    parameter int N = 4
) (
    input logic clk,
    input logic a,
    output logic q
);
  localparam int M = N + 1;
  parameter int P = 2; //~ STY014
  logic [3:0] r;
  int i;
  always_ff @(posedge clk) q <= a; //~ STY013
  always_ff @(posedge clk) begin
    if (a) r <= 0; //~ STY013
    else if (r == 1) begin
      r <= 2;
    end else r <= r + 1; //~ STY013
  end
  initial begin
    for (i = 0; i < 2; i++) $display("0x%h", r); //~ STY013
    while (i > 0) begin
      i--;
    end
    if ($test$plusargs("dump")) begin //~ SUS004
      $display("%h", r); //~ STY015
      $display("%b %0d 0b%b 'h%h %%h", r, i, r, r); //~ STY015
      $display("0x%d", r); //~ STY015
    end
  end
endmodule
