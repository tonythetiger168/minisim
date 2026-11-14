// IEEE 1800-2023 10.10, 7.9.11: string literals in an unpacked array
// concatenation fill a string array with the strings, and the default of
// an associative array of strings given by '{default: "..."} is what a
// missing key reads.
module t;
  string s[] = {"hello", "sad", "world"};
  string q[$] = {"x", "y"};
  string w[int] = '{default: "none"};
  string v[string];
  initial begin
    string l[] = {"p", "q"};
    string f[$];
    $display("%0d %s %s %s | %s %s | %s %s", s.size(), s[0], s[1], s[2], q[0], q[1], l[0], l[1]);
    f = s.find with (item == "sad");
    $display("%0d %s", f.size(), f[0]);
    s.reverse();
    $display("%s %s %s", s[0], s[1], s[2]);
    w[1] = "one";
    $display("%s %s %0d", w[1], w[7], w.num());
    v = '{"a": "A", default: "?"};
    $display("%s %s", v["a"], v["zz"]);
  end
endmodule
//> 3 hello sad world | x y | p q
//> 1 sad
//> world sad hello
//> one none 1
//> A ?
//! icarus-skip: Icarus does not parse string array concatenations
