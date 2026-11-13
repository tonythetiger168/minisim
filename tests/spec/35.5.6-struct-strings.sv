// IEEE 1800-2023 35.5.6 / H.7.6: a string is a permitted DPI type, so an
// unpacked struct argument may have string members; C sees each as a
// `const char*`, in and out, also in an array of such structs.
//! args: data/dpi/ustrings.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  typedef struct {
    string   name;
    int      n;
    shortint tag;
    string   note;
  } item_t;
  import "DPI-C" function int c_show(input item_t it);
  import "DPI-C" function void c_fill(input int k, output item_t it);
  import "DPI-C" function void c_upper(inout item_t it);
  import "DPI-C" function int c_count(input item_t its[3], input int n);
  item_t a, b;
  item_t many[3];
  initial begin
    a = '{name: "alpha", n: 300, tag: -2, note: "first"};
    $display("len %0d", c_show(a));
    a.name = {a.name, "-beta"};
    $display("len %0d", c_show(a));
    for (int k = 1; k <= 2; k++) begin
      c_fill(k, b);
      $display("%s n=%0d tag=%0d %s", b.name, b.n, b.tag, b.note);
    end
    c_upper(b);
    $display("%s n=%0d %s", b.name, b.n, b.note);
    many = '{'{"a", 1, 0, ""}, '{"bb", 2, 0, ""}, '{"ccc", 3, 0, "x"}};
    $display("count %0d", c_count(many, 3));
  end
endmodule
//> C: alpha n=300 tag=-2 note=first
//> len 10
//> C: alpha-beta n=300 tag=-2 note=first
//> len 15
//> item1 n=-1 tag=100 odd
//> item2 n=-2 tag=200 even
//> ITEM2 n=-1 even
//> count 14
