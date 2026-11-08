// IEEE 1800-2023 6.16: string variables start empty; concatenation, len,
// substr, comparison; 21.3.3 $sformatf.
module top;
  string s, t = "World", u;
  integer n = 42;
  initial begin
    $display("[%s] %0d", s, s.len());
    s = "Hello";
    u = {s, ", ", t, "!"};
    $display("%s %0d", u, u.len());
    $display("%s|%s|", u.substr(0, 4), u.substr(7, 11));
    $display("%0d %0d", s == "Hello", s < t);
    u = $sformatf("v=%0d h=%h", n, 8'hab);
    $display("%s", u);
    if (t == "World") $display("eq");
  end
endmodule

//> [] 0
//> Hello, World! 13
//> Hello|World|
//> 1 1
//> v=42 h=ab
//> eq
