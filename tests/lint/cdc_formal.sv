// -cdc-formal: three 4-bit buses through 2-flop synchronizers.
//   gray_q  a Gray-coded pointer (+0 or +1 per clock): proved one bit per clock.
//   skip_q  Gray-coded, but the pointer steps by 2: two bits change (CDC010).
//   john_q  a Johnson counter: not Gray by its look (CDC002 without
//           -cdc-formal), but proved to change one bit per clock.
module cdc_formal (
  input  logic       clk_a, clk_b, rst_ni,
  input  logic       inc,
  output logic [3:0] g_b, s_b, j_b
);
  function automatic logic [3:0] bin2gray(logic [3:0] b);
    return b ^ (b >> 1);
  endfunction

  logic [3:0] bin_q, gray_q, skip_bin_q, skip_q, john_q;
  logic [3:0] skip_next, skip_gray;
  assign skip_next = skip_bin_q + 4'd2;
  assign skip_gray = skip_next ^ (skip_next >> 1);

  always_ff @(posedge clk_a or negedge rst_ni)
    if (!rst_ni) begin
      bin_q <= '0;
      gray_q <= '0;
      skip_bin_q <= '0;
      skip_q <= '0;
      john_q <= '0;
    end else if (inc) begin
      bin_q <= bin_q + 4'd1;
      gray_q <= bin2gray(bin_q + 4'd1);
      skip_bin_q <= skip_next;
      skip_q <= skip_gray;
      john_q <= {john_q[2:0], ~john_q[3]};
    end

  logic [3:0] g1, s1, j1;
  always_ff @(posedge clk_b or negedge rst_ni)
    if (!rst_ni) begin
      g1 <= '0; g_b <= '0;
      s1 <= '0; s_b <= '0;
      j1 <= '0; j_b <= '0;
    end else begin
      g1 <= gray_q; g_b <= g1;
      s1 <= skip_q; s_b <= s1;
      j1 <= john_q; j_b <= j1;
    end
endmodule
