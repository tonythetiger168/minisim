// IEEE 1800-2023 15.4: A mailbox: put, get, try_get, num, and a bounded
// mailbox that blocks (15.4).
//! icarus-skip: Icarus 13 does not support mailboxes
module top;
  mailbox #(int) m;
  int v;
  initial begin
    m = new;
    m.put(3); m.put(4);
    $display("%0d", m.num());
    m.get(v); $display("%0d", v);
    $display("%0d %0d", m.try_get(v), v);
    $display("%0d", m.try_get(v));
    fork
      begin m.get(v); $display("%0d at %0t", v, $time); end
      #2 m.put(9);
    join
  end
endmodule

//> 2
//> 3
//> 1 4
//> 0
//> 9 at 2
