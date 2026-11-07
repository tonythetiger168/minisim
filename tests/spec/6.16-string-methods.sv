//! icarus-skip: Icarus 12 lacks most string methods (toupper, getc, compare, putc, itoa)
// IEEE 1800-2023 6.16.1-6.16.15: string methods; 21.3.3 $sformat; string
// arrays and functions; integral to string conversion; case on a string.
module top;
  string s, t = "World", u;
  string names[0:1];
  integer n;
  bit [31:0] b;
  function string greet(input string who); return {"Hi ", who}; endfunction
  initial begin
    $display("[%s] %0d", s, s.len());
    s = "Hello";
    u = {s, ", ", t, "!"};
    $display("%s %0d", u, u.len());
    $display("%s %s", u.toupper(), u.tolower());
    $display("%s|%s|", u.substr(0, 4), u.substr(7, 11));
    $display("%c %0d", u.getc(1), u.getc(1));
    $display("%0d %0d %0d", s == "Hello", s < t, s.compare(t));
    s.putc(0, "J");
    $display("%s", s);
    n = 42;
    s.itoa(n); $display("%s", s);
    s.hextoa(255); $display("%s", s);
    s = "123"; $display("%0d", s.atoi() + 1);
    s = "ff"; $display("%0d", s.atohex());
    u = $sformatf("v=%0d h=%h", n, 8'hab);
    $display("%s", u);
    $sformat(u, "%0d-%0d", 1, 2);
    $display(u);
    names[1] = greet("Ann");
    $display("%s", names[1]);
    b = "abcd";
    s = string'(b);
    $display("%s %h", s, b);
    $display("%s", {2{"ab"}});
    if (t == "World") $display("eq");
    case (t)
      "World": $display("case hit");
      default: $display("case miss");
    endcase
  end
endmodule

//> [] 0
//> Hello, World! 13
//> HELLO, WORLD! hello, world!
//> Hello|World|
//> e 101
//> 1 1 -1
//> Jello
//> 42
//> ff
//> 124
//> 255
//> v=42 h=ab
//> 1-2
//> Hi Ann
//> abcd 61626364
//> abab
//> eq
//> case hit
