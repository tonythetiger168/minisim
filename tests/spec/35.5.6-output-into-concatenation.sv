// IEEE 1800-2023 35.5.1.2, 11.4.12: a DPI import's output argument with a
// concatenation as its actual. After the call each part takes its slice
// of the value the C function wrote.
//! args: data/dpi/out_concat.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  typedef enum bit [3:0] {S0 = 0, SC = 12} st_e;
  import "DPI-C" function void fill16(output bit [15:0] d);
  bit [7:0] a;
  st_e e;
  bit [3:0] b;
  initial begin
    fill16({a, e, b});
    $display("a=%h e=%s b=%h", a, e.name(), b);
  end
endmodule
//> a=a5 e=SC b=3
