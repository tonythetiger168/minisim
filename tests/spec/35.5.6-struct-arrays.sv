// IEEE 1800-2023 35.5.6, H.7.6: an unpacked array of unpacked structs is a
// DPI argument too: a sized formal is a C array of the C structs, an open
// one an svOpenArrayHandle whose elements are those structs.
//! args: data/dpi/structarr.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  typedef struct { shortint x; real w; int tag; } item_t;
  import "DPI-C" function real weigh(input item_t a[3]);
  import "DPI-C" function void scale(inout item_t a[]);
  item_t it[3];
  item_t dyn[];
  initial begin
    it = '{'{1, 1.5, 10}, '{2, 2.0, 20}, '{-3, 0.5, 30}};
    $display("%0.2f", weigh(it));
    dyn = new[2];
    dyn[0] = '{5, 1.0, 0}; dyn[1] = '{-7, 2.25, 0};
    scale(dyn);
    $display("%p", dyn);
    scale(it);
    $display("%0d %0.2f %0d", it[2].x, it[2].w, it[2].tag);
  end
endmodule
//> 64.00
//> C: 2 items of 24 bytes
//> '{'{x:10, w:1.50000, tag:0}, '{x:-14, w:2.75000, tag:1}}
//> C: 3 items of 24 bytes
//> -6 1.00 2
