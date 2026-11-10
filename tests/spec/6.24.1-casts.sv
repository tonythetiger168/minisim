// IEEE 1800-2023 6.24.1: Type, size and signing casts (6.24.1).
module top;
  logic [7:0] a = 8'hff;
  logic [3:0] n = 4'hf;
  initial begin
    $display("%0d %0d", int'(3.7), int'(-2.5));
    $display("%h %h", 4'(a), 12'(a));
    $display("%0d %0d", signed'(n), unsigned'(-4'sd1));
  end
endmodule

//> 4 -3
//> f 0ff
//> -1 15
