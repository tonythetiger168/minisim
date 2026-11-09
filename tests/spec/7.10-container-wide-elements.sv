// IEEE 1800-2023 7.5, 7.10: containers of unpacked structs and of vectors
// wider than 64 bits; writes to a field or a part of an element.
//! icarus-skip: Icarus 13 does not support unpacked structs
typedef struct { int a; byte b; logic [99:0] wide; } s_t;
module top;
  s_t q[$];
  s_t one;
  logic [99:0] w[];
  int k = 1;
  initial begin
    one.a = 5; one.b = 2; one.wide = 100'h1_2345_6789_abcd_ef01_2345_6789;
    q.push_back(one);
    q.push_back('{a: 7, b: 1, wide: 0});
    q[1].b = 9;
    q[k].wide[3:0] = 4'ha;
    $display("%0d %0d %0d %h %0d %h", q.size(), q[0].a, q[1].b, q[0].wide, q[1].a, q[1].wide);
    w = new[2];
    w[1] = 100'hf_0000_0000_0000_0000_0000_0001;
    w[1][99] = 0;
    w[k][k] = 1;
    $display("%h", w[1]);
  end
endmodule

//> 2 5 9 123456789abcdef0123456789 7 000000000000000000000000a
//> 7000000000000000000000003
