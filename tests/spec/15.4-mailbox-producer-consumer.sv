// IEEE 1800-2023 15.4: a producer and a consumer through a bounded
// mailbox of depth 1, in a class (a testbench driver pattern), and a
// typeless mailbox (15.4.9).
//! icarus-skip: Icarus 13 does not support mailboxes
class Chan;
  mailbox #(int) mb;
  function new(); mb = new(1); endfunction
  task send(int x); mb.put(x); endtask
  task recv(output int x); mb.get(x); endtask
endclass

module top;
  Chan c = new;
  mailbox any = new;
  int sum, x;
  initial begin
    fork
      for (int i = 1; i <= 4; i++) begin c.send(i); $display("sent %0d at %0t", i, $time); end
      repeat (4) begin #3 c.recv(x); sum += x; end
    join
    $display("sum %0d at %0t", sum, $time);
    any.put(7);
    any.get(x);
    $display("%0d %0d", x, any.num());
  end
endmodule

//> sent 1 at 0
//> sent 2 at 3
//> sent 3 at 6
//> sent 4 at 9
//> sum 10 at 12
//> 7 0
