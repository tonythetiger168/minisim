// IEEE 1800-2023 7.6 and 23.3.3: unpacked arrays as module ports (input and
// output, connected to an array or an assignment pattern), and a continuous
// assignment of a whole unpacked array.
module sub (input logic [7:0] in [3], output logic [7:0] out [3], output logic [9:0] sum);
  assign out = '{in[2], in[1], in[0]};
  assign sum = in[0] + in[1] + in[2];
endmodule
module top;
  logic [7:0] a [3], b [3], c [3];
  wire [7:0] w [3];
  logic [9:0] s1, s2;
  assign w = a;
  sub u1 (.in(a), .out(b), .sum(s1));
  sub u2 (.in('{8'd1, 8'd2, 8'd3}), .out(c), .sum(s2));
  initial begin
    a = '{8'd10, 8'd20, 8'd30};
    #1 $display("%0d %0d %0d | %0d %0d %0d | %0d %0d", b[0], b[1], b[2], c[0], c[1], c[2], s1, s2);
    $display("%0d %0d %0d", w[0], w[1], w[2]);
    a[1] = 8'd99;
    #1 $display("%0d %0d", b[1], s1);
  end
endmodule
//> 30 20 10 | 3 2 1 | 60 6
//> 10 20 30
//> 99 139
