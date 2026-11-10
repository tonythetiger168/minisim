// Crossings mlint recognizes as safe: nothing to report.
module sync2 #(parameter int N = 2) (input logic clk, rst_n, d, output logic q);
  logic [N-1:0] s;
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) s <= '0;
    else        s <= {s[N-2:0], d};
  assign q = s[N-1];
endmodule

module cdc_clean(input logic aclk, bclk, arst_n, input logic [7:0] din, input logic send,
                 output logic [7:0] dout, output logic [3:0] rptr_a, output logic busy,
                 output logic div_q);
  // Each domain releases the reset through its own synchronizer.
  logic a_rst_n, b_rst_n, ar1, br1;
  always_ff @(posedge aclk or negedge arst_n)
    if (!arst_n) {a_rst_n, ar1} <= '0; else {a_rst_n, ar1} <= {ar1, 1'b1};
  always_ff @(posedge bclk or negedge arst_n)
    if (!arst_n) {b_rst_n, br1} <= '0; else {b_rst_n, br1} <= {br1, 1'b1};

  // Domain A holds the data and toggles a request.
  logic [7:0] hold;
  logic req;
  always_ff @(posedge aclk or negedge a_rst_n)
    if (!a_rst_n) begin hold <= '0; req <= 1'b0; end
    else if (send) begin hold <= din; req <= ~req; end

  // Domain B: the request through a synchronizer module, then an edge detect
  // that qualifies the bus (mux recirculation).
  logic req_b, req_b_q;
  sync2 #(.N(3)) u_req(.clk(bclk), .rst_n(b_rst_n), .d(req), .q(req_b));
  always_ff @(posedge bclk or negedge b_rst_n)
    if (!b_rst_n) begin req_b_q <= 1'b0; dout <= '0; end
    else begin
      req_b_q <= req_b;
      if (req_b ^ req_b_q) dout <= hold;
    end

  // A Gray-coded pointer from B back to A, in a loop of flops.
  logic [3:0] bin, gray;
  wire  [3:0] bin_next = bin + 4'd1;
  always_ff @(posedge bclk or negedge b_rst_n)
    if (!b_rst_n) begin bin <= '0; gray <= '0; end
    else begin bin <= bin_next; gray <= bin_next ^ (bin_next >> 1); end
  logic [3:0] gsync [2];
  always_ff @(posedge aclk) begin
    gsync[0] <= gray;
    for (int i = 1; i < 2; i++) gsync[i] <= gsync[i-1];
  end
  assign rptr_a = gsync[1];

  // A gated clock is still aclk: no crossing.
  logic en_l, gated_q;
  always_latch if (!aclk) en_l = send;
  wire gclk = aclk & en_l;
  always_ff @(posedge gclk) gated_q <= req;
  assign busy = gated_q;

  // A divided clock is its own domain, unless -clock-group says otherwise.
  logic div;
  always_ff @(posedge aclk or negedge a_rst_n)
    if (!a_rst_n) div <= 1'b0; else div <= ~div;
  always_ff @(posedge div or negedge a_rst_n)
    if (!a_rst_n) div_q <= 1'b0; else div_q <= req;
endmodule
