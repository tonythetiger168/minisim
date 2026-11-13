// IEEE 1800-2023 9.2.2.2.1 / 10.3.2: the implicit sensitivity of a continuous
// assignment or always_comb holds what it reads, not the arguments, result
// and locals of the functions it calls. Two processes that call one function
// (which writes its result bit by bit) must not wake each other for ever.
module top;
  function automatic logic [1:0] g(input logic [3:0] d);
    g[0] = d[0] ^ d[1];
    g[1] = d[2] ^ d[1];
  endfunction
  function automatic logic [3:0] find(input logic [1:0] syn);
    find = '0;
    for (int i = 0; i < 4; i++) if (syn == g(4'h1 << i)) find = 4'h1 << i;
  endfunction
  logic [3:0] h = 0;
  wire  [1:0] s   = g(h);
  wire  [3:0] fix = find(s);
  logic [1:0] c;
  always_comb c = g(~h);
  initial begin
    #1 $display("%h %h %h", s, fix, c);
    h = 2;
    #1 $display("%h %h %h", s, fix, c);
    h = 4;
    #1 $display("%h %h %h", s, fix, c);
  end
endmodule
//> 0 8 0
//> 3 2 3
//> 2 4 2
