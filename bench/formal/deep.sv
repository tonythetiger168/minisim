module deep(input logic clk, rst, input logic en, input logic [3:0] k);
  logic [23:0] c; logic [15:0] lf;
  always_ff @(posedge clk) if (rst) begin c <= 0; lf <= 16'hace1; end else if (en) begin c <= c + 1'b1; lf <= {lf[14:0], lf[15]^lf[13]^lf[12]^lf[10]}; end
  a_lf: assert property (@(posedge clk) disable iff (rst) lf != 0);
  a_c: assert property (@(posedge clk) disable iff (rst) c != 24'd300);
  a_par: assert property (@(posedge clk) disable iff (rst) en |=> c[0] != $past(c[0]));
endmodule
