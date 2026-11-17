// IEEE 1800-2023 15.5.5 and 13.5: an event passed to a task or function,
// by value or by ref, names the caller's synchronization object, so the
// callee can wait on it, trigger it or read .triggered. The actual may be
// a module event, a class event property, an element of an event array or
// null (an event that is never triggered).
//! icarus-skip: Icarus 13 does not accept event arguments
class Port;
  event done;
  task watch(event x); @x; $display("port saw %0t", $time); endtask
endclass
module top;
  event go, lanes[3];
  Port p = new;
  task waiter(event x); @x; $display("static saw %0t", $time); endtask
  task automatic fire(event x); ->x; endtask
  task automatic fire_ref(ref event x); #1 ->x; endtask
  function automatic bit hit(event x); return x.triggered; endfunction
  task automatic wait_on(event x); wait (x.triggered); $display("wait saw %0t", $time); endtask
  initial begin
    fork waiter(go); p.watch(p.done); wait_on(lanes[1]); join_none
    #1 fire(go);
    #1 fire(p.done);
    #1 fire(lanes[1]);
    $display("hit %0d %0d %0d", hit(lanes[1]), hit(lanes[2]), hit(null));
    fork begin @go; $display("ref saw %0t", $time); end join_none
    fire_ref(go);
  end
endmodule
//> static saw 1
//> port saw 2
//> hit 1 0 0
//> wait saw 3
//> ref saw 4
