// IEEE 1800-2023 35.5.6: Every DPI argument and result type maps to its C
// type: byte, shortint, int, longint, real, bit, logic, string, chandle, and
// packed vectors in canonical form; outputs and inouts are pointers (35.5.6,
// H.7).
//! args: data/dpi/types.c
//! icarus-skip: Icarus 13 does not support DPI
module top;
  import "DPI-C" function byte c_byte(input byte a);
  import "DPI-C" function shortint c_short(input shortint a);
  import "DPI-C" function int c_add(input int a, input int b);
  import "DPI-C" function longint c_long(input longint a);
  import "DPI-C" function real c_real(input real a, input real b);
  import "DPI-C" function bit c_not(input bit b);
  import "DPI-C" function logic c_logic(input logic l);
  import "DPI-C" function string c_greet(input string name);
  import "DPI-C" function int c_strlen(input string s);
  import "DPI-C" function chandle c_handle();
  import "DPI-C" function int c_bump(input chandle h);
  import "DPI-C" function void c_outs(input int a, output int twice, output real half,
                                      output bit odd, output string name);
  import "DPI-C" function void c_inc(inout int x, inout byte b);
  import "DPI-C" function void c_vec(input bit [69:0] i, output bit [69:0] o);
  import "DPI-C" function void c_lvec(input logic [7:0] i, output logic [7:0] o);
  import "DPI-C" function int c_bits(input bit [39:0] v);
  import "DPI-C" function void c_part(input logic [15:0] v, output logic [7:0] hi);
  import "DPI-C" function int c_integer(input integer v);
  typedef enum int {RED = 3, GREEN = 7} color_t;

  int t; real h; bit o; string s; int x; byte b; chandle ch;
  bit [69:0] v70; logic [7:0] l8; logic [7:0] hi;
  initial begin
    $display("%0d %0d", c_byte(8'sd60), c_byte(-8'sd3));
    $display("%0d", c_short(-16'sd32768 + 16'sd1));
    $display("%0d %0d", c_add(2, 40), c_add(-5, 3));
    $display("%0d", c_long(64'sd7));
    $display("%0.3f", c_real(1.0, 8.0));
    $display("%b %b", c_not(1'b0), c_not(1'b1));
    $display("%b%b%b%b", c_logic(1'b0), c_logic(1'b1), c_logic(1'bz), c_logic(1'bx));
    s = c_greet("dpi");
    $display("%s %0d", s, c_strlen("abcdef"));
    ch = c_handle();
    $display("%0d %0d %0d", ch != null, c_bump(ch), c_bump(ch));
    c_outs(7, t, h, o, s);
    $display("%0d %0.1f %b %s", t, h, o, s);
    c_outs(4, t, h, o, s);
    $display("%0d %0.1f %b %s", t, h, o, s);
    x = 41; b = 8'sd126;
    c_inc(x, b);
    $display("%0d %0d", x, b);
    c_inc(x, b);
    $display("%0d %0d", x, b);
    v70 = 70'h2a_0000_ffff_1234_5678;
    c_vec(v70, v70);
    $display("%h", v70);
    c_lvec(8'b10zx_01x1, l8);
    $display("%b", l8);
    $display("%0d", c_bits(40'hf0_0000_0003));
    c_part(16'b1x0z_0011_1111_0000, hi);
    $display("%b", hi);
    $display("%0d %0d", c_integer(5), c_integer(32'bx));   // x is aval 1, bval 1
    $display("%0d", c_add(GREEN, RED));
  end
endmodule

//> 120 -6
//> -32768
//> 42 -2
//> 7000000000
//> 0.125
//> 1 0
//> 01zx
//> hello dpi 6
//> 1 1 2
//> 14 3.5 1 odd
//> 8 2.0 0 even
//> 42 127
//> 43 -128
//> 15ffff0000edcba987
//> 01x110zx
//> 6
//> 1x0z0011
//> 5 999
//> 10
