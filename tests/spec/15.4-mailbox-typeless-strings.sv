// IEEE 1800-2023 15.4.9: a typeless mailbox holds messages of any type; a
// string, given as a literal or a variable, comes back out as that string.
//! icarus-skip: Icarus 13 has no mailboxes
module top;
  mailbox m = new;
  string s, t = "world";
  int x;
  initial begin
    m.put("hello");
    m.put(5);
    m.put(t);
    m.get(s);
    m.get(x);
    $display("%s %0d", s, x);
    m.get(s);
    $display("%s %0d", s, m.num());
  end
endmodule

//> hello 5
//> world 0
