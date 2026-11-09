// IEEE 1800-2023 11.4.12.1 Replication operator: a replication with a
// zero count inside a concatenation is ignored.
module t #(parameter int N = 0);
  logic [3:0] a = 4'b1011;
  logic [3:0] b;
  assign b = {{N{1'b0}}, a};
  initial begin
    #1 $display("%b %b", b, {a, {N{1'b1}}, 2'b01});
  end
endmodule
//> 1011 101101
