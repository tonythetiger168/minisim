// IEEE 1800-2023 10.9.1: in an array pattern, default: may give a pattern
// with keys of its own ('{default: '{default: 2}}), filling each element of
// a multidimensional array. When the elements are unpacked structs and the
// default is a simple value, every member of every element gets it ('1 fills
// each member's bits; nested structs and member arrays too).
//! icarus-skip: Icarus does not support these assignment patterns
module top;
  typedef struct { int i; logic [7:0] b; } leaf_t;
  typedef struct { leaf_t l; bit [3:0] v[2]; } node_t;
  int m[2][3];
  leaf_t s[2][2];
  node_t n[2];
  initial begin
    m = '{0: '{0: 1, default: 3}, default: '{default: 2}};
    $display("%p", m);
    s = '{default: '1};
    $display("%p", s[1][0]);
    s = '{default: 5};
    $display("%p", s[0][1]);
    n = '{default: '0};
    $display("%p", n);
  end
endmodule
//> '{'{1, 3, 3}, '{2, 2, 2}}
//> '{i:-1, b:255}
//> '{i:5, b:5}
//> '{'{l:'{i:0, b:0}, v:'{0, 0}}, '{l:'{i:0, b:0}, v:'{0, 0}}}
