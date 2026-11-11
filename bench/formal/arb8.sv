module arb8 #(parameter N=8)(input logic clk, rst, input logic [N-1:0] req, output logic [N-1:0] gnt);
  logic [$clog2(N)-1:0] last;
  always_comb begin gnt = '0;
    for (int i = N; i >= 1; i--) begin automatic logic [$clog2(N)-1:0] k = last + i[$clog2(N)-1:0]; if (req[k]) gnt = N'(1) << k; end
  end
  always_ff @(posedge clk) if (rst) last <= 0; else for (int i = 0; i < N; i++) if (gnt[i]) last <= i[$clog2(N)-1:0];
  a_onehot: assert property (@(posedge clk) disable iff (rst) $onehot0(gnt));
  a_sub: assert property (@(posedge clk) disable iff (rst) (gnt & ~req) == 0);
  a_some: assert property (@(posedge clk) disable iff (rst) |req |-> |gnt);
  for (genvar g = 0; g < N; g++) begin : f
    m_hold: assume property (@(posedge clk) disable iff (rst) req[g] && !gnt[g] |=> req[g]);
    a_live: assert property (@(posedge clk) disable iff (rst) req[g] |-> s_eventually gnt[g]);
    a_bound: assert property (@(posedge clk) disable iff (rst) req[g] |-> ##[0:N-1] gnt[g]);
  end
endmodule
