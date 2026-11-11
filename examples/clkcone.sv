// The clock and reset reach the registers through port connections, so
// each instance waits on its own copy. The copies form the clock cone, and
// the clocked processes of all instances run as one cycle domain.
module cc_stage #(parameter int K = 1) (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [15:0] d_i,
  output logic [15:0] q_o,
  output logic        par_o
);
  logic [15:0] nxt;
  logic [47:0] wide;
  always_comb begin
    nxt = d_i + 16'(K);
    if (d_i[0]) nxt = {nxt[7:0], nxt[15:8]};
  end
  assign wide  = {q_o, d_i, nxt};
  assign par_o = ^{q_o, d_i[10:0], nxt, wide[40:3]};   // a reduction over more than 64 bits
  // Three clock groups: with a reset, without one, and on the other edge.
  logic [15:0] hold, neg;
  always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) q_o <= '0;
    else         q_o <= nxt ^ {16{d_i[3]}} ^ hold ^ neg;
  always_ff @(posedge clk_i) hold <= d_i ^ 16'h00ff;
  always_ff @(negedge clk_i) neg  <= q_o + 16'd1;
endmodule

module cc_mid (input logic clk_i, input logic rst_ni, input logic [15:0] d_i, output logic [15:0] q_o, output logic [1:0] p_o);
  logic [15:0] m;
  cc_stage #(.K(3)) a (.clk_i, .rst_ni, .d_i,      .q_o(m),   .par_o(p_o[0]));
  cc_stage #(.K(5)) b (.clk_i, .rst_ni, .d_i(m),   .q_o,      .par_o(p_o[1]));
endmodule

module clkcone;
  logic clk = 0, rst_n = 0;
  logic [15:0] cnt, q;
  logic [1:0]  p;
  logic [31:0] sum;
  cc_mid u (.clk_i(clk), .rst_ni(rst_n), .d_i(cnt), .q_o(q), .p_o(p));
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) begin cnt <= '0; sum <= '0; end
    else begin cnt <= cnt + 16'd7; sum <= sum * 32'd31 + {14'd0, p, q}; end
  always #5 clk = ~clk;
  initial begin
    #22 rst_n = 1;
    #3000;
    $display("sum=%h q=%h p=%b cnt=%h", sum, q, p, cnt);
    $finish;
  end
endmodule
