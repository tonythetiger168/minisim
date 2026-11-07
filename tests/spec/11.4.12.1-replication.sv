// IEEE 1800-2023 11.4.12.1 Replication operator.
module t;
  logic [7:0] a;
  logic [1:0] p = 2'b10;
  initial begin
    a = {4{p}};
    $display("%b %b", a, {2{3'b101}});
  end
endmodule
//> 10101010 101101
