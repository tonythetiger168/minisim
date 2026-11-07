//! icarus-skip: Icarus 13 has no named or default assignment patterns
// IEEE 1800-2023 10.9.2: '{member: v}, '{v, v}, '{default: v}, '{n{v}} for packed types.
module top;
  typedef struct packed { logic [3:0] hi; logic [3:0] lo; } byte_t;
  byte_t s;
  logic [2:0][3:0] v;
  initial begin
    s = '{hi: 4'h1, lo: 4'h2};
    $display("%h", s);
    s = '{4'h7, 4'h8};
    $display("%h", s);
    s = '{default: 4'hc};
    $display("%h", s);
    v = '{3{4'h5}};
    $display("%h", v);
  end
endmodule

//> 12
//> 78
//> cc
//> 555
