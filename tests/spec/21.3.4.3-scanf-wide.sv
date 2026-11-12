// IEEE 1800-2023 21.3.4.3, 21.6: $sscanf, $fscanf and $value$plusargs read
// numbers wider than 64 bits in full with %h, %b, %o and %d; the digits of
// %h, %b and %o are zero-extended into a wider variable, %d sign-extends.
//! args: +W=0123456789abcdef0123456789abcdef +I=-42
module top;
  logic [127:0] y, b;
  logic [131:0] o;
  logic [99:0] d;
  logic signed [127:0] sd;
  logic [71:0] p;
  int fd, n;
  initial begin
    n = $sscanf("f0e1d2c3b4a5968778695a4b3c2d1e0f", "%h", y);
    $display("%0d %h", n, y);
    n = $sscanf("ffffffffffffffff", "%h", y);
    $display("%0d %h", n, y);
    n = $sscanf("1x_z0f00000000000000000000", "%h", y);
    $display("%0d %h", n, y);
    n = $sscanf("1100110011001100110011001100110011001100110011001100110011001100101", "%b", b);
    $display("%0d %h", n, b);
    n = $sscanf("7654321076543210765432107", "%o", o);
    $display("%0d %o", n, o);
    n = $sscanf("987654321098765432109876543210", "%d", d);
    $display("%0d %0d", n, d);
    n = $sscanf("-987654321098765432109876543210", "%d", sd);
    $display("%0d %0d", n, sd);
    n = $sscanf("fedcba9876543210ab 77", "%h %h", p, y);
    $display("%0d %h %h", n, p, y);
    fd = $fopen("spec_21_3_4_3.txt", "w");
    $fdisplay(fd, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
    $fclose(fd);
    fd = $fopen("spec_21_3_4_3.txt", "r");
    n = $fscanf(fd, "%h", y);
    $fclose(fd);
    $display("%0d %h", n, y);
    if ($value$plusargs("W=%h", y)) $display("W %h", y);
    if ($value$plusargs("I=%d", sd)) $display("I %0d", sd);
  end
endmodule
//> 1 f0e1d2c3b4a5968778695a4b3c2d1e0f
//> 1 0000000000000000ffffffffffffffff
//> 1 00000001xz0f00000000000000000000
//> 1 00000000000000066666666666666665
//> 1 00000000000000000007654321076543210765432107
//> 1 987654321098765432109876543210
//> 1 -987654321098765432109876543210
//> 2 fedcba9876543210ab 00000000000000000000000000000077
//> 1 0f1e2d3c4b5a69788796a5b4c3d2e1f0
//> W 0123456789abcdef0123456789abcdef
//> I -42
