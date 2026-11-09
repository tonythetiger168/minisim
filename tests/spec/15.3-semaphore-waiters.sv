// IEEE 1800-2023 15.3: semaphore keys: new() with no keys, put of several
// keys at once, get blocking until enough keys are there, try_get of more
// keys than there are (15.3.1-15.3.4).
//! icarus-skip: Icarus 13 does not support semaphores
module top;
  semaphore s = new;
  int order[$];
  initial begin
    $display("%0d", s.try_get());        // no keys yet
    fork
      begin s.get(2); order.push_back(2); $display("two at %0t", $time); end
      begin #1 s.get(1); order.push_back(1); $display("one at %0t", $time); end
    join_none
    #5 s.put(1);                          // not enough for the get(2)
    #5 s.put(2);
    #1 $display("%0d %0d", s.try_get(3), s.try_get(0));
    s.put(4);
    $display("%0d", s.try_get(4));
    $display("%0d", s.try_get(1));
  end
endmodule

//> 0
//> one at 5
//> two at 10
//> 0 1
//> 1
//> 0
