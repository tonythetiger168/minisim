// IEEE 1800-2023 21.2.1 Display and write tasks: %b %o %h %d with and without
// a 0 width, an explicit width, x/z digits, $write without newline, and
// $displayh / $displayb defaults for arguments with no specifier.
module t;
  logic [7:0] a = 8'hab;
  logic [7:0] z = 8'b1zzz_xx01;
  logic [7:0] five = 5;
  initial begin
    $display("%b %o %h %d", a, a, a, a);
    $display("%0b|%0h|%0o|%0d|%5d|%3h", five, five, five, five, five, five);
    $display("%h %b", z, z);
    $write("w1 ");
    $write("w2\n");
    $display(five, "|", a);
    $displayh(a);
    $displayb(five);
    $display("100%%");
  end
endmodule
//> 10101011 253 ab 171
//> 101|5|5|5|    5| 05
//> ZX 1zzzxx01
//> w1 w2
//>   5|171
//> ab
//> 00000101
//> 100%
