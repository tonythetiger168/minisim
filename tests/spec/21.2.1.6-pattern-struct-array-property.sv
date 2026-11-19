// IEEE 1800-2023 21.2.1.6: %p prints an unpacked array of unpacked structs as
// nested assignment patterns, also when the array is a class property or a
// struct member reached through a handle (each element by member name).
//! icarus-skip: Icarus does not print unpacked structs with %p
typedef struct { bit [6:0] value; struct { bit [6:0] value; } inner; } entry_t;
class Holder;
  entry_t slots[2];
  struct { entry_t e[2]; int n; } pack;
endclass
module top;
  Holder h;
  initial begin
    h = new;
    h.slots[0].value = 1;
    h.slots[1].inner.value = 5;
    h.pack.e[1].value = 9;
    h.pack.n = 4;
    $display("%p", h.slots);
    $display("%p", h.slots[1]);
    $display("%p", h.pack.e);
  end
endmodule
//> '{'{value:1, inner:'{value:0}}, '{value:0, inner:'{value:5}}}
//> '{value:0, inner:'{value:5}}
//> '{'{value:0, inner:'{value:0}}, '{value:9, inner:'{value:0}}}
