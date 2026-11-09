// IEEE 1800-2023 21.2.1.6: %p prints a value as an assignment pattern:
// arrays and packed structs as patterns, enums by name, strings in quotes,
// other values unformatted (21.2.1.6).
//! icarus-skip: Icarus 13 does not print %p of arrays
module top;
  int a[3] = '{1, 2, 3};
  int m[2][2] = '{'{1, 2}, '{3, 4}};
  int q[$] = '{4, 5};
  string sq[$] = '{"a", "b"};
  int aa[int];
  typedef struct packed { logic [3:0] x, y; } s_t;
  s_t s = '{4'h1, 4'h2};
  typedef enum {ON, OFF} e_t;
  e_t e = OFF;
  e_t ea[2] = '{OFF, ON};
  string str = "hi";
  initial begin
    aa[20] = 2; aa[10] = 1;
    $display("%p", a);
    $display("%p", m);
    $display("%p", q);
    $display("%p", sq);
    $display("%p", aa);
    $display("%p", s);
    $display("e=%p ea=%p str=%p n=%0p", e, ea, str, 8'd7);
  end
endmodule

//> '{1, 2, 3}
//> '{'{1, 2}, '{3, 4}}
//> '{4, 5}
//> '{"a", "b"}
//> '{10:1, 20:2}
//> '{x:1, y:2}
//> e=OFF ea='{OFF, ON} str="hi" n=7
