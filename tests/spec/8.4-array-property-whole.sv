// IEEE 1800-2023 8.4 with 7.4 and 21.2.1.6: a fixed-size array property used
// as a whole through a handle: assignment from a pattern (also one that
// reads the array itself, so the values are taken first), a nonblocking
// assignment, and %p.
//! icarus-skip: Icarus 13 does not print %p of arrays
class C; bit [3:0] fa[3]; int g[2][2]; endclass
module top; C c = new;
  initial begin
    c.fa = '{1, 2, 3}; c.g = '{'{1, 2}, '{3, 4}};
    $display("%p %p", c.fa, c.g);
    c.fa = '{c.fa[2], c.fa[1], c.fa[0]};
    c.g <= '{default: 7};
    #1 $display("%p %p", c.fa, c.g);
  end
endmodule

//> '{1, 2, 3} '{'{1, 2}, '{3, 4}}
//> '{3, 2, 1} '{'{7, 7}, '{7, 7}}
