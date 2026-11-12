// IEEE 1800-2023 5.9, 5.9.1: string literals longer than 8 characters, a
// literal truncated on the left or padded with zeros on the left to fit
// its vector, escape sequences (\t \" \\ \v \f \a \ddd \xdd), a line
// continued with a backslash, and triple-quoted literals that hold
// newlines and quotes.
//! icarus-skip: Icarus 13 has no triple-quoted string literals
module top;
  reg [8*20:1] r;
  reg [8*4:1] t;
  string s;
  initial begin
    r = "a long string literal";
    $display("%s|%0d", r, $bits("abcdefghijklmnop"));
    if ("abcdefghij" == 80'h6162636465666768696a) $display("eq");
    t = "toolongvalue";
    $display("%s|%h", t, t);
    t = "ab";
    $display("%h", t);
    $display("%h", "\x41\101\t\"\\\v\f\a");
    $display("a\
b");
    s = """multi
line "quoted" text""";
    $display("%s|%0d", s, s.len());
  end
endmodule
//>  long string literal|128
//> eq
//> alue|616c7565
//> 00006162
//> 414109225c0b0c07
//> ab
//> multi
//> line "quoted" text|24
