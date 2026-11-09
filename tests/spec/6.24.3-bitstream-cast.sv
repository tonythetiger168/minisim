// IEEE 1800-2023 6.24.3: A bitstream cast between a packed struct and a
// vector (6.24.3).
module top;
  typedef struct packed { logic [3:0] a, b; } s_t;
  s_t s;
  initial begin
    s = s_t'(8'h5a);
    $display("%h %h", s.a, s.b);
  end
endmodule

//> 5 a
