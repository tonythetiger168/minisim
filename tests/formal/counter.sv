// mformal test: a counter. tests/mformal.cmake has the expected verdicts.
module counter(input logic clk, input logic rst_n, input logic en, output logic [3:0] cnt, output logic wrap);
  always_ff @(posedge clk or negedge rst_n)
    if (!rst_n) cnt <= 4'd0;
    else if (en) cnt <= (cnt == 4'd9) ? 4'd0 : cnt + 4'd1;
  assign wrap = cnt == 4'd9 && en;
  // proven: never above 9
  a_range: assert property (@(posedge clk) disable iff (!rst_n) cnt <= 4'd9);
  // falsified: reaches 5
  a_lt5: assert property (@(posedge clk) disable iff (!rst_n) cnt < 4'd5);
  // proven: wrap implies zero next
  a_wrap: assert property (@(posedge clk) disable iff (!rst_n) wrap |=> cnt == 4'd0);
  // proven with $past
  a_step: assert property (@(posedge clk) disable iff (!rst_n) en && cnt != 4'd9 |=> cnt == $past(cnt) + 4'd1);
  c_nine: cover property (@(posedge clk) cnt == 4'd9);
  c_twelve: cover property (@(posedge clk) cnt == 4'd12);
  c_seq: cover property (@(posedge clk) cnt == 4'd2 ##1 cnt == 4'd3 ##1 cnt == 4'd3 ##1 cnt==4'd4);
endmodule
