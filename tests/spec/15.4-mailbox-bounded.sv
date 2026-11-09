// IEEE 1800-2023 15.4: a bounded mailbox: put blocks while it is full,
// try_put fails, peek and try_peek leave the message in place, messages
// keep their order, and a mailbox can carry class handles and strings
// (15.4.1-15.4.8).
//! icarus-skip: Icarus 13 does not support mailboxes
class Pkt;
  int id;
  function new(int i); id = i; endfunction
endclass

module top;
  mailbox #(int) m = new(2);
  mailbox #(Pkt) mp = new;
  mailbox #(string) ms = new;
  int v;
  Pkt p;
  string s;
  initial begin
    m.put(1); m.put(2);
    $display("%0d %0d", m.try_put(3), m.num());
    fork
      begin m.put(3); $display("put 3 at %0t", $time); end
      begin #4 m.get(v); $display("got %0d at %0t", v, $time); end
    join
    $display("%0d", m.num());
    m.peek(v); $display("peek %0d num %0d", v, m.num());
    v = 0;
    $display("%0d %0d", m.try_peek(v), v);
    m.get(v); m.get(v); $display("last %0d", v);
    $display("%0d %0d", m.try_peek(v), m.try_get(v));
    for (int i = 0; i < 3; i++) begin p = new(10 + i); mp.put(p); end
    p = null;
    while (mp.try_get(p)) $display("pkt %0d", p.id);
    ms.put("hello"); ms.put("world");
    ms.get(s); $write("%s ", s); ms.get(s); $display("%s", s);
  end
endmodule

//> 0 2
//> got 1 at 4
//> put 3 at 4
//> 2
//> peek 2 num 2
//> 1 2
//> last 3
//> 0 0
//> pkt 10
//> pkt 11
//> pkt 12
//> hello world
